// SPDX-FileCopyrightText: © 2024 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0
#include "lower_to_buda/netlist.hpp"

#include <sstream>

#include "buda_passes.hpp"
#include "graph_lib/graph.hpp"
#include "graph_lib/node.hpp"
#include "graph_lib/node_types.hpp"
#include "graph_lib/utils.hpp"
#include "lower_to_buda/debug.hpp"
#include "lower_to_buda/graph.hpp"
#include "lower_to_buda/program.hpp"
#include "lower_to_buda/queue.hpp"
#include "passes/forked_dram_inputs.hpp"
#include "passes/fuse_ops.hpp"
#include "placer/utils.hpp"
#include "utils/assert.hpp"
#include "utils/logger.hpp"

namespace tt
{

using Graph = graphlib::Graph;
using Node = graphlib::Node;
using NodeType = graphlib::NodeType;
using Edge = graphlib::Edge;

void dump_group_of_queues(
    std::stringstream &ss, const std::string &type, const std::vector<BudaQueue> &qs, std::size_t longest_name)
{
    if (qs.size() > 0)
    {
        ss << std::endl << "  " << "# " << type << std::endl;
        for (const BudaQueue &q : qs)
        {
            ss << "  " << q.as_string(longest_name) << std::endl;
        }
    }
}

std::string BudaNetlist::dump_to_yaml() const
{
    std::stringstream ss;

    ss << comments;
    if (comments)
        ss << std::endl;  // Add an extra newline for readability

    ss << debug_info;

    ss << "devices:" << std::endl;
    ss << "  arch: " << arch_string << std::endl << std::endl;

    ss << "queues:" << std::endl;

    std::size_t longest_name = 0;
    for (const BudaQueue &q : queues)
    {
        if (q.name.length() > longest_name)
            longest_name = q.name.length();
    }

    // Group by type, for nicer display
    std::unordered_map<std::string, std::vector<BudaQueue>> grouped;

    for (const BudaQueue &q : queues)
    {
        grouped[q.type].push_back(q);
    }

    // Fixed order of common types
    std::vector<std::string> types = {"input", "output", "parameter", "constant", "epoch_to_epoch", "accumulator"};

    for (std::string type : types)
    {
        dump_group_of_queues(ss, type, grouped[type], longest_name);
        grouped.erase(type);
    }

    // Print other groups that might've showed up that we don't know about
    for (auto const &[type, qs] : grouped)
    {
        dump_group_of_queues(ss, type, qs, longest_name);
    }

    ss << std::endl;
    ss << "graphs:" << std::endl;
    for (const BudaGraph &g : graphs)
    {
        ss << g;
    }

    ss << std::endl;
    ss << "programs:" << std::endl;
    for (const program::Program &p : programs)
    {
        ss << "  - " << p;
    }

    ss << std::endl;

    if (fused_ops.size() > 0)
    {
        ss << "fused_ops:" << std::endl;
        for (const BudaFusedOp &op : fused_ops) ss << "  " << op;
    }

    return ss.str();
}

std::string get_buda_queue_type(graphlib::QueueNode *node)
{
    if (node->node_type() == graphlib::NodeType::kInput)
    {
        graphlib::InputNode *input_node = node->as<graphlib::InputNode>();
        return input_node->input_type_string();
    }

    return node->queue_type_string();
}

// Common code for all queues
BudaQueue create_queue(
    Graph *graph,
    graphlib::QueueNode *node,
    placer::QueuePlacement const &placement,
    balancer::BlockShapeMap const &block_shape_map)
{
    std::string type = get_buda_queue_type(node);
    std::string memory_access = node->memory_access_type_string();

    BudaQueue q(placement.name, type, memory_access, placement.chip_id, node->shape().get_tile_dim());

    q.input_name = placement.input_name;
    q.entries = node->get_num_entries();
    q.microbatch = graph->get_microbatch();
    q.data_format = node->output_df();
    q.alias = node->get_alias();
    q.dims = {
        .grid_r = (int)placement.grid_shape.rows,
        .grid_c = (int)placement.grid_shape.columns,
    };

    if (placement.on_host)
    {
        q.loc = BudaQueueLocation::HOST;
        for (placer::QueueHostBufferPlacement const &p : placement.host_buffers)
        {
            q.host_loc.push_back({p.channel, p.address});
        }
    }
    else
    {
        q.loc = BudaQueueLocation::DRAM;
        for (placer::QueueBufferPlacement const &p : placement.dram_buffers)
        {
            q.dram_loc.push_back({p.dram_channel, p.dram_address});
        }
    }

    if ((node->node_type() == NodeType::kOutput and q.loc == BudaQueueLocation::HOST) or
        node->node_type() != NodeType::kQueue)
    {
        q.blocks = block_shape_map.at(node->name()).as_buda_blocks();
    }
    else
    {
        // e2e queue inserted after balancer, inherit from producer
        Node *producer = graph->data_operands(node)[0];
        q.blocks = block_shape_map.at(producer->name()).as_buda_blocks();
    }

    switch (node->node_type())
    {
        case NodeType::kInput:
        {
            q.layout = node->as<graphlib::InputNode>()->get_layout();
            q.ublock_order = graphlib::get_input_queue_ublock_order(graph, node);
            break;
        }
        case NodeType::kOutput:  // fallthrough
        case NodeType::kQueue:   // fallthrough
        {
            auto producer = graph->data_operands(node);
            TT_ASSERT(producer.size() == 1);
            q.ublock_order = get_output_ublock_order(graph, producer[0]);
            break;
        }
        default:
        {
            TT_ASSERT(false, "Unhandled queue node type", node->node_type());
            break;
        }
    }

    return q;
}

void validate_op_grid_size(BudaOp const &op)
{
    if (op.type == "ethernet_datacopy")
        return;

    TT_ASSERT(op.grid.grid_size_c > 0 && op.grid.grid_size_r > 0, "Op {} has a 0 in grid size", op.name);
}

static BudaOp create_op(
    Graph *graph,
    graphlib::BudaOpNode *node,
    std::vector<BudaOperand> const &operands,
    std::vector<tt::DataFormat> &input_df,
    BudaOpAttrs const &buda_attrs,
    placer::OpPlacement const &placement,
    std::unordered_map<std::string, placer::QueuePlacement> const &name_to_queue_placement,
    balancer::OpModel const &op_model,
    balancer::BlockShape const &block_shape,
    std::string const &arch_name,
    std::vector<std::size_t> const &input_dram_io_buf_size_tiles,
    const std::vector<graphlib::Edge> &forked_dram_edges,
    bool ignore_tms = false)
{
    BudaOp op;

    op.name = node->name();
    op.type = node->op_type().op;
    op.gradient_op = node->is_gradient_op();
    op.input_data_formats = input_df;
    op.output_data_format = node->output_df();
    op.intermediate_data_format = node->intermediate_df();
    op.accumulate_data_format = node->accumulate_df();
    op.fidelity = node->math_fidelity();
    op.attrs = buda_attrs;
    op.tile_dim = node->shape().get_tile_dim();

    if (!forked_dram_edges.empty())
    {
        for (auto edge : forked_dram_edges)
        {
            Node *producer_node = nullptr;
            Node *consumer_node = nullptr;
            producer_node = graph->node_by_id(edge.producer_node_id);
            consumer_node = graph->node_by_id(edge.consumer_node_id);
            // forked_dram_inputs holds following pair info {QueueNode name,BudaOpNode name}
            op.forked_dram_inputs.push_back({producer_node->name(), consumer_node->name()});
        }
    }

    // For fused op override op.attr with global attributes.
    // Specific op attributes will be added later for each fused sub op.
    if (node->is_fused_op())
    {
        op.attrs = node->get_fused_op()->get_operation_attr();
    }

    op.untilize_output = false;
    for (Edge e : graph->user_data_edges(node))
    {
        Node *output = graph->node_by_id(e.consumer_node_id);
        if (output->node_type() == graphlib::NodeType::kOutput)
        {
            try
            {
                placer::QueuePlacement qp = name_to_queue_placement.at(output->name());
                if (qp.on_host)
                {
                    op.untilize_output = true;  // ops that are writing to host need to untilize their output
                }
            }
            catch (std::out_of_range &e)
            {
                throw std::runtime_error(
                    "Queue missing in placement results for " + output->name() + ", something went wrong.");
            }
        }
    }

    TT_ASSERT(
        !node->op_type().buda_attrs.empty() or node->op_type().attr.size() == 0 or node->is_fused_op(),
        "All ops with attributes should've been lowered to something else by this point.",
        OStreamJoin("node:", node->name()),
        OStreamJoin("type:", node->op_type().op));

    try
    {
        placer::Coord s = placement.placed_cores.start;
        placer::Coord e = placement.placed_cores.end;
        placer::Coord op_grid_size = {.row = e.row - s.row, .col = e.col - s.col};
        op.grid = (placement.grid_transpose) ? BudaOpGrid{s.row, s.col, op_grid_size.col, op_grid_size.row}
                                             : BudaOpGrid{s.row, s.col, op_grid_size.row, op_grid_size.col};
        op.grid_transpose = placement.grid_transpose;

        validate_op_grid_size(op);

        auto operand_edges = graph->operand_data_edges(node);
        std::vector<std::uint32_t> input_buf_min_size_tiles;
        bool input_buf_overrides = false;
        for (std::size_t i = 0; i < operand_edges.size(); i++)
        {
            const Edge &operand = operand_edges[i];
            auto edge_attrs = graph->get_edge_attributes(operand);
            std::vector<graphlib::OpType> tms = edge_attrs->get_tms();
            if (tms.size() > 0 and not ignore_tms)
            {
                // Tile broadcasts are fused into the fused ops themselves
                std::vector<graphlib::OpType> filtered_tms;
                std::copy_if(
                    begin(tms),
                    end(tms),
                    std::back_inserter(filtered_tms),
                    [](auto op_type) { return op_type.op != "tile_broadcast"; });
                if (filtered_tms.size() < tms.size())
                    TT_ASSERT(
                        node->op_type().op == "fused_op",
                        "Only fused ops should have tile broadcast TMs: {}",
                        node->name());
                op.tms.insert(std::make_pair(operand.consumer_input_port_id, filtered_tms));
            }

            // We could just get the number from the input buffer model, but we don't want to pollute
            // the netlist with "default" values - we only want to list something when there's an override
            if (op_model.parameter_buffers.at(i).is_unrolled())
            {
                input_buf_overrides = true;
                input_buf_min_size_tiles.push_back(op_model.parameter_buffers.at(i).size_tiles(true /*include_t*/));
                TT_ASSERT(not op_model.input_buffers.at(i).size_tiles_override);
            }
            else if (op_model.input_buffers.at(i).is_unrolled())
            {
                input_buf_overrides = true;
                input_buf_min_size_tiles.push_back(op_model.input_buffers.at(i).size_tiles(false /*include_t*/));
            }
            else
            {
                input_buf_min_size_tiles.push_back(0);
            }

            for (std::size_t i = 0; i < op_model.input_buffers.size(); i++)
                if (op_model.input_buffers[i].minimize_input_buffer)
                {
                    op.attrs["min_buffer_input"] = (int)i;
                    break;
                }
        }

        if (input_buf_overrides)
            op.input_buf_min_size_tiles = input_buf_min_size_tiles;

        op.ublock_order = get_output_ublock_order(graph, node);
        op.blocks = block_shape.as_buda_blocks();
        // TODO: ensure that there's actually room? it's probably not the best place to do this
        op.buf_size_mb = op_model.get_output_buffer_factor();

        // Overlay blob
        op.overlay_size = op_model.overlay_size;
    }
    catch (std::out_of_range &e)
    {
        throw std::runtime_error("Op missing in placement results for " + node->name() + ", something went wrong.");
    }

    TT_ASSERT(
        not op.untilize_output or op.ublock_order == graphlib::UBlockOrder::R,
        "Untilizer requires row-major ublock ordering");

    op.inputs = operands;
    op.input_dram_io_buf_size_tiles = input_dram_io_buf_size_tiles;

    std::stringstream ss;
    to_debug_info(ss, node->name(), op_model, arch_name, input_dram_io_buf_size_tiles);
    op.debug_info = Comment(ss);

    return op;
}

static std::tuple<BudaOp, std::vector<BudaOperand>, std::vector<tt::DataFormat>, BudaOpAttrs, placer::OpPlacement>
create_sparse_buffer_op(
    Graph *graph,
    graphlib::BudaOpNode *node,
    std::vector<BudaOperand> operands,
    std::vector<tt::DataFormat> const &input_df,
    BudaOpAttrs const &buda_attrs,
    placer::OpPlacement const &placement,
    std::unordered_map<std::string, placer::QueuePlacement> const &name_to_queue_placement,
    balancer::OpModel const &op_model,
    balancer::BlockShape const &block_shape,
    std::string const &arch_name,
    std::vector<std::size_t> const &input_dram_io_buf_size_tiles)
{
    TT_ASSERT(operands.size() == 3);
    TT_ASSERT(input_df.size() == 3);
    std::vector<BudaOperand> buffer_operands = {operands[1], operands[2]};
    std::vector<tt::DataFormat> buffer_input_df = {input_df[1], input_df[2]};
    // Fixup op attributes
    BudaOpAttrs sparse_mm_buda_attrs = buda_attrs;
    BudaOpAttrs buffer_buda_attrs = buda_attrs;
    buffer_buda_attrs.erase("num_sparse_tiles");
    buffer_buda_attrs.erase("identity");
    buffer_buda_attrs.erase("act_buffered");
    if (buda_attrs.find("kernel_broadcast") != buda_attrs.end())
    {
        TT_ASSERT(std::get<std::vector<int>>(buda_attrs.at("kernel_broadcast"))[0] == 1);
        buffer_buda_attrs["kernel_broadcast"] = std::vector<int>({0});
        sparse_mm_buda_attrs.erase("kernel_broadcast");
    }
    // Fixup block shape
    balancer::BlockShape buffer_block_shape = block_shape;
    buffer_block_shape.mblock_m = 1;
    buffer_block_shape.ublock.rt = op_model.input_buffers[1].block_shape.ublock.rt;
    std::vector<tt::DataFormat> sparse_mm_input_df = input_df;
    sparse_mm_input_df[1] = node->output_df();
    // Fixup placement
    placer::OpPlacement buffer_placement = placement;
    placer::OpPlacement sparse_mm_placement = placement;
    TT_ASSERT((buffer_placement.placed_cores.size_c() % 2) == 0);
    std::uint32_t split_grid_c = buffer_placement.placed_cores.size_c() / 2;
    buffer_placement.placed_cores.end.col = buffer_placement.placed_cores.start.col + split_grid_c;
    sparse_mm_placement.placed_cores.start.col = buffer_placement.placed_cores.end.col;
    std::vector<graphlib::Edge> forked_dram_edges;
    BudaOp op = create_op(
        graph,
        node,
        buffer_operands,
        buffer_input_df,
        buffer_buda_attrs,
        buffer_placement,
        name_to_queue_placement,
        op_model,
        buffer_block_shape,
        arch_name,
        input_dram_io_buf_size_tiles,
        forked_dram_edges,
        false);

    op.name = node->name() + "_buffer";
    op.type = "buffer";
    operands[1] = op.name;
    // Remap index 1 tms to index 0
    if (op.tms.find(1) != op.tms.end())
    {
        op.tms[0] = op.tms[1];
        op.tms.erase(1);
    }

    return std::make_tuple(op, operands, sparse_mm_input_df, sparse_mm_buda_attrs, sparse_mm_placement);
}

BudaFusedOp create_fused_op(graphlib::BudaOpNode *op, const balancer::OpModel &op_model)
{
    TT_ASSERT(op->is_fused_op());

    std::vector<std::vector<BudaFusedSubOp>> schedules;
    std::uint32_t input_count = 0;
    std::vector<DataFormat> intermed_df;
    std::unordered_map<std::uint32_t, balancer::UBlockShape> intermed_ublock_shape;
    balancer::UBlockShape dest_ublock_shape;

    // TODO: a lot of these calculations don't belong here, but in some kind of post-placer pass
    // This should be a straight data transfer from op attributes to netlist attributes.
    for (auto sch : op->get_fused_op()->get_schedules())
    {
        std::vector<BudaFusedSubOp> schedule;
        for (FusedSubOp f : sch.ops)
        {
            std::vector<std::string> inputs;
            std::vector<balancer::UBlockShape> input_block_shapes;
            std::unordered_map<int, std::vector<graphlib::OpType>> tms;  // per operand
            std::uint32_t sub_op_input_count = 0;
            for (auto i : f.inputs)
            {
                tms.insert(std::make_pair(sub_op_input_count, std::vector<graphlib::OpType>{}));
                if (i.type == FusedSubOpInput::InputType::INPUT)
                {
                    inputs.push_back("input" + std::to_string(i.index));
                    if (i.index + 1 > input_count)
                        input_count = i.index + 1;
                    input_block_shapes.push_back(op_model.input_buffers[i.index].block_shape.ublock);
                }
                else if (i.type == FusedSubOpInput::InputType::DEST)
                {
                    inputs.push_back("dest");
                    input_block_shapes.push_back(dest_ublock_shape);
                }
                else
                {
                    inputs.push_back("intermed" + std::to_string(i.index));
                    if (i.broadcast.second > 0)
                    {
                        graphlib::OpType brcst(
                            "broadcast", std::vector<BudaOpAttr>{(int)i.broadcast.first, (int)i.broadcast.second});
                        tms[sub_op_input_count].push_back(brcst);
                    }
                    input_block_shapes.push_back(intermed_ublock_shape.at(i.index));
                }
                if (i.tile_broadcast.first)
                {
                    tms[sub_op_input_count].push_back(graphlib::OpType("tile_broadcast", {2}, {}));
                }
                else if (i.tile_broadcast.second)
                {
                    tms[sub_op_input_count].push_back(graphlib::OpType("tile_broadcast", {3}, {}));
                }
                sub_op_input_count++;
            }
            std::string output = "intermed" + std::to_string(f.output_buffer);
            balancer::UBlockShape u_shape = op_model.fused_op_ublock_shape.at(f.name);

            if (f.output_type == FusedSubOp::OutputType::OUTPUT)
            {
                output = "output";  // final output
            }
            else if (f.output_type == FusedSubOp::OutputType::DEST)
            {
                output = "dest";  // dest reuse
                dest_ublock_shape = u_shape;
            }
            else
            {
                if (intermed_df.size() < (std::uint32_t)f.output_buffer + 1)
                    intermed_df.resize(f.output_buffer + 1);
                intermed_df[f.output_buffer] = f.output_df;
                intermed_ublock_shape.insert(std::make_pair(f.output_buffer, u_shape));
            }
            // std::pair<std::uint32_t, std::uint32_t> ublock_shape = {blocks.ublock_rt, blocks.ublock_ct};
            balancer::Parallelization par = op_model.parallelization();
            std::pair<std::uint32_t, std::uint32_t> ublock_shape = {u_shape.rt, u_shape.ct};
            std::pair<std::uint32_t, std::uint32_t> block_shape =
                f.get_mblock_for_ublock(ublock_shape, std::make_pair(par.r, par.c));

            if (f.op_type.op == "matmul")
            {
                balancer::UBlockShape input0_ublock = input_block_shapes.at(0);
                balancer::UBlockShape input1_ublock = input_block_shapes.at(1);
                std::uint32_t m_k = f.op_shape.inputs.at(0).ct / std::max(input0_ublock.ct, input1_ublock.rt);
                if (m_k == 0)
                    m_k = 1;
                std::uint32_t u_kt = f.op_shape.inputs.at(0).ct / m_k;
                f.op_type.buda_attrs["m_k"] = (int)m_k;
                f.op_type.buda_attrs["u_kt"] = (int)u_kt;
            }
            // TODO: Should we also somehow merge attr to buda_attr?
            auto sub_op = BudaFusedSubOp{
                f.name,
                f.op_type.op,
                inputs,
                f.get_sub_op_buda_attr(),
                tms,
                output,
                f.popped_buffers,
                f.popped_last_buffers,
                block_shape,
                ublock_shape};

            schedule.push_back(sub_op);
        }
        schedules.push_back(schedule);
    }

    return BudaFusedOp{op->get_fused_op()->id(), input_count, intermed_df, schedules};
}

BudaNaryTM create_nary_tm(graphlib::BudaNaryTMNode *node, std::vector<BudaOperand> const &operands)
{
    BudaNaryTM tm;
    tm.name = node->name();
    tm.type = node->op_type().op;
    tm.inputs = operands;
    return tm;
}

// Given a linearized list of global epoch-ids, get a 2d-list capturing the
// temporal-special relation
std::vector<std::vector<std::uint32_t>> get_temporal_to_concurrent_spatial_epochs(
    const placer::PlacerSolution &placer_solution, const std::vector<std::uint32_t> &epochs)
{
    std::vector<std::vector<std::uint32_t>> temporal_to_spatial_epochs(placer_solution.num_temporal_epochs());
    for (std::uint32_t global_epoch_id : epochs)
    {
        uint32_t temporal_epoch_id = placer_solution.temporal_epoch_id(global_epoch_id);
        TT_ASSERT(temporal_epoch_id < temporal_to_spatial_epochs.size());
        temporal_to_spatial_epochs[temporal_epoch_id].push_back(global_epoch_id);
    }
    return temporal_to_spatial_epochs;
}

std::pair<int, int> get_epoch_allocate_deallocate(graphlib::Node *q, const placer::PlacerSolution &placer_solution)
{
    try
    {
        const auto &qp = placer_solution.name_to_queue_placement.at(q->name());
        return std::make_pair(qp.epoch_allocate, qp.epoch_deallocate);
    }
    catch (std::out_of_range &e)
    {
        throw std::runtime_error("Queue missing in placement results for " + q->name() + ", something went wrong.");
    }
}

std::vector<program::Program> create_programs(
    Graph *graph, placer::PlacerSolution &placer_solution, BudaGraph &buda_graph, const std::string &arch_string)
{
    std::vector<program::Program> programs;

    auto bwd_epochs = buda_graph.get_matching_epoch(graphlib::NodeEpochType::Backward);
    bool have_bwd_epochs = bwd_epochs.size() > 0;
    bool have_opt_epochs = buda_graph.get_matching_epoch(graphlib::NodeEpochType::Optimizer).size() > 0;

    std::uint32_t last_bwd_epoch = 0;
    if (have_bwd_epochs)
    {
        last_bwd_epoch = *std::max_element(bwd_epochs.begin(), bwd_epochs.end());
    }

    bool firmware_looping_enabled = env_as<int>("NUM_EXEC_LOOP_ITERATIONS", 0) > 1;

    for (unsigned int subgraph_index = 0; subgraph_index < graph->num_subgraphs(); subgraph_index++)
    {
        for (graphlib::NodeEpochType epoch_type :
             {graphlib::NodeEpochType::Forward, graphlib::NodeEpochType::Backward, graphlib::NodeEpochType::Optimizer})
        {
            // Get fwd epochs
            std::vector<std::uint32_t> epochs = buda_graph.get_matching_epoch(epoch_type);

            if (!have_bwd_epochs && (epoch_type != graphlib::NodeEpochType::Forward))
                continue;

            // For each epoch, find the input and e2e nodes that feed it
            std::vector<std::vector<graphlib::Node *>> input_queues;
            for (std::uint32_t epoch : epochs)
            {
                input_queues.push_back(graph->nodes(
                    [&graph, &placer_solution, epoch](Node *node)
                    {
                        if ((node->node_type() != graphlib::NodeType::kInput) &&
                            (node->node_type() != graphlib::NodeType::kQueue) &&
                            (node->node_type() != graphlib::NodeType::kOutput))
                            return false;

                        std::vector<graphlib::Edge> edges;
                        bool check_producer = false;
                        if ((node->node_type() == graphlib::NodeType::kInput) ||
                            (node->node_type() == graphlib::NodeType::kQueue))
                            edges = graph->user_data_edges(node);
                        else if (node->node_type() == graphlib::NodeType::kOutput)
                        {
                            edges = graph->operand_data_edges(node);
                            check_producer = true;
                        }
                        // If there's any edge matching, we should record the connection between queue and epoch
                        for (Edge edge : edges)
                        {
                            Node *neighbour =
                                graph->node_by_id(check_producer ? edge.producer_node_id : edge.consumer_node_id);
                            try
                            {
                                if (
                                    // Our epoch
                                    (placer_solution.name_to_op_placement.at(neighbour->name()).epoch_id() == epoch) &&

                                    (
                                        // Input
                                        ((node->node_type() == graphlib::NodeType::kInput) &&
                                         (node->as<graphlib::InputNode>()->is_activation() ||
                                          node->as<graphlib::InputNode>()->is_loss() ||
                                          node->as<graphlib::InputNode>()->is_target()))

                                        // Or e2e
                                        || ((node->node_type() == graphlib::NodeType::kQueue) &&
                                            node->as<graphlib::QueueNode>()->is_epoch_to_epoch())

                                        // Or output with loopback edge
                                        || ((node->node_type() == graphlib::NodeType::kOutput) &&
                                            not graph
                                                    ->user_edges(
                                                        node,
                                                        [](Edge e)
                                                        { return e.edge_type == graphlib::EdgeType::kPartialDataCopy; })
                                                    .empty())

                                        // Or buffering queue
                                        || ((node->node_type() == graphlib::NodeType::kQueue) &&
                                            node->as<graphlib::QueueNode>()->is_buffering())))
                                    return true;
                            }
                            catch (std::out_of_range &e)
                            {
                                throw std::runtime_error(
                                    "Op missing in placement results for " + neighbour->name() +
                                    ", something went wrong.");
                            }
                        }

                        return false;
                    }));
            }

            // For each epoch, find the parameter and constant queues that feed it
            std::vector<std::vector<graphlib::Node *>> parameter_queues;
            for (std::uint32_t epoch : epochs)
            {
                parameter_queues.push_back(graph->nodes(
                    [&graph, &placer_solution, epoch](Node *node)
                    {
                        if (node->node_type() != graphlib::NodeType::kInput)
                            return false;

                        // If there's any user matching, we should record the connection between queue and epoch
                        for (Edge user_edge : graph->user_data_edges(node))
                        {
                            Node *user = graph->node_by_id(user_edge.consumer_node_id);
                            try
                            {
                                if (
                                    // Our epoch
                                    (placer_solution.name_to_op_placement.at(user->name()).epoch_id() == epoch) &&
                                    ((node->as<graphlib::InputNode>()->is_parameter()) ||
                                     (node->as<graphlib::InputNode>()->is_constant())))
                                    return true;
                            }
                            catch (std::out_of_range &e)
                            {
                                throw std::runtime_error(
                                    "Op missing in placement results for " + user->name() + ", something went wrong.");
                            }
                        }
                        return false;
                    }));
            }

            // For each epoch, find the gradient queues
            std::vector<std::vector<graphlib::Node *>> gradient_queues;
            for (std::uint32_t epoch : epochs)
            {
                gradient_queues.push_back(graph->nodes(
                    [&graph, &placer_solution, epoch, have_opt_epochs](Node *node)
                    {
                        if ((node->node_type() != graphlib::NodeType::kQueue) ||
                            (!node->as<graphlib::QueueNode>()->is_grad_accumulator()))
                            return false;

                        TT_ASSERT(
                            graph->operand_data_edges(node).size() == 1,
                            "Grad accumulator " + node->name() + " should have exactly one producer");
                        Node *producer = graph->node_by_id(graph->operand_data_edges(node)[0].producer_node_id);

                        Node *consumer = nullptr;
                        if (have_opt_epochs)
                        {
                            TT_ASSERT(
                                graph->user_data_edges(node).size() == 1,
                                "Grad accumulator " + node->name() +
                                    " should have exactly one consumer when optimizer is used");
                            consumer = graph->node_by_id(graph->user_data_edges(node)[0].consumer_node_id);
                        }

                        try
                        {
                            return
                                // Bwd
                                ((placer_solution.name_to_op_placement.at(producer->name()).epoch_id() == epoch) &&
                                 producer->as<graphlib::BudaOpNode>()->is_gradient_op()) ||

                                // Optimizer
                                ((consumer != nullptr) &&
                                 (placer_solution.name_to_op_placement.at(consumer->name()).epoch_id() == epoch));
                        }
                        catch (std::out_of_range &e)
                        {
                            throw std::runtime_error(
                                "Op missing in placement results for " + producer->name() + ", something went wrong.");
                        }
                    }));
            }

            //
            // Generate queue settings for each epoch execute
            //
            std::vector<std::vector<program::QueueSettings>> queue_settings;
            std::vector<std::set<std::tuple<std::string, program::VariableP, std::uint32_t, bool>>>
                increment_list;  // list of ptrs to increment after execute, with wrap values

            // Variable map for queues
            // We'll reuse variable for queues in the same epoch, of same type, and same size
            int qvar_index = 0;
            std::unordered_map<
                std::uint32_t,  // epoch
                std::unordered_map<
                    std::string,  // type
                    std::unordered_map<
                        std::uint32_t,  // size
                        std::unordered_map<
                            bool,  // static
                            std::pair<program::VariableP, program::VariableP>>>>>
                qvars_map;
            auto qvars = [&qvars_map, &qvar_index](
                             graphlib::Node *node, int epoch, program::Variable::ShadowType shadow_type, bool is_static)
            {
                std::string type = "";
                std::uint32_t size = node->as<graphlib::QueueNode>()->get_num_entries();

                if (node->node_type() == graphlib::NodeType::kQueue)
                    type = "e2e";
                else if (node->node_type() == graphlib::NodeType::kInput)
                    type = node->as<graphlib::InputNode>()->input_type_string();

                if (shadow_type == program::Variable::ShadowType::CROSS_EPOCH)
                    type += "_ce_ng";
                else if (shadow_type == program::Variable::ShadowType::CROSS_PROGRAM)
                    type += "_cp_ng";

                if ((qvars_map[epoch].find(type) == qvars_map[epoch].end()) ||
                    (qvars_map[epoch][type].find(size) == qvars_map[epoch][type].end()) ||
                    (qvars_map[epoch][type][size].find(is_static) == qvars_map[epoch][type][size].end()))
                {
                    // Node doesn't have variables, let's create some
                    qvars_map[epoch][type][size].insert(std::make_pair(
                        is_static,
                        std::make_pair(
                            std::make_shared<program::Variable>("lptr_q" + std::to_string(qvar_index), is_static),
                            std::make_shared<program::Variable>(
                                "gptr_q" + std::to_string(qvar_index), is_static, 0, shadow_type))));
                    qvar_index++;
                }
                return qvars_map[epoch].at(type).at(size).at(is_static);
            };

            // Generate settings
            std::vector<program::VariableP> queue_variables;
            bool has_cache_buffers = false;
            bool has_cache_read_buffer = false;
            std::unordered_map<std::uint32_t, std::uint32_t> epoch_to_epoch_index;
            for (std::uint32_t epoch_index = 0; epoch_index < epochs.size(); epoch_index++)
            {
                std::uint32_t epoch = epochs[epoch_index];
                epoch_to_epoch_index[epoch] = epoch_index;
                queue_settings.push_back({});
                increment_list.push_back({});
                if (placer_solution.epoch_id_to_subgraph_index[epoch] != subgraph_index)
                    continue;

                // All input should be read locally. In the last epoch that reads the input, it should be read
                // globally, too.
                uint32_t microbatch_size = graph->get_microbatch();
                for (graphlib::Node *q : input_queues[epoch_index])
                {
                    if (q->as<graphlib::QueueNode>()->is_buffering())
                    {
                        placer::QueuePlacement qp = placer_solution.name_to_queue_placement.at(q->name());
                        bool queue_static = qp.is_static();
                        // If queue is static
                        if (queue_static)
                        {
                            // If (buffering) queue is statically allocated, its num_entries has to be at least
                            // microbatch_size, because
                            //  global_rdptr_autoinc is false.
                            uint32_t num_entries = q->as<graphlib::QueueNode>()->get_num_entries();
                            TT_ASSERT(
                                num_entries >= microbatch_size,
                                "Buffering queue {} has num_entries: {}, but microbatch_size is: {}. Since queue is "
                                "static it needs to have num_entries at least microbatch size.",
                                q->name(),
                                num_entries,
                                microbatch_size);
                            // Need to increment static queue rd/wtr ptrs as queue is persistant
                            uint32_t temporal_epoch_id = placer_solution.temporal_epoch_id(epoch);
                            const auto &[lptr, gptr] =
                                qvars(q, temporal_epoch_id, program::Variable::ShadowType::NONE, true);

                            auto qs = program::QueueSettings(
                                q->name(),
                                program::QueueAttributes{
                                    .read_ptr_global_ = gptr,
                                    .read_ptr_local_ = lptr,
                                    .epoch_allocate = -1,
                                    .epoch_deallocate = -1});

                            std::uint32_t wrap_value = q->as<graphlib::QueueNode>()->get_num_entries();
                            increment_list[epoch_index].insert(std::make_tuple(lptr->name(), lptr, wrap_value, true));
                            increment_list[epoch_index].insert(std::make_tuple(gptr->name(), gptr, wrap_value, true));
                            queue_settings[epoch_index].push_back(qs);
                        }
                        else
                        {
                            // We do not increment any of the rd/wtr ptrs for buffering queues because
                            // queue will be produced and consumed within the same epoch execution
                            auto qs = program::QueueSettings(
                                q->name(),
                                program::QueueAttributes{
                                    .read_ptr_global_ = nullptr,
                                    .read_ptr_local_ = nullptr,
                                    .epoch_allocate = (int)epoch,
                                    .epoch_deallocate = (int)epoch});
                            // since buffering queue is produced and consumed within the same epoch, we alway enable
                            // global autoincrement
                            qs.global_rdptr_autoinc = true;
                            queue_settings[epoch_index].push_back(qs);
                        }
                        continue;
                    }

                    uint32_t temporal_epoch_id = placer_solution.temporal_epoch_id(epoch);
                    bool read_global;
                    if (q->as<graphlib::QueueNode>()->is_output())
                    {
                        read_global = (temporal_epoch_id == get_first_epoch_producer(graph, q, placer_solution));
                    }
                    else
                    {
                        read_global = (temporal_epoch_id == get_last_epoch_use(graph, q, placer_solution));
                    }
                    auto [epoch_allocate, epoch_deallocate] = get_epoch_allocate_deallocate(q, placer_solution);

                    // WORKAROUND: If we have a backward, we need a "shadow global read pointer" to track the
                    // real one when fwd-bwd-fwd-bwd are called, otherwise setting it to 0 will "reset" it for bwd.
                    // This needs to be removed eventually, as it only works around this particular call pattern.
                    bool needs_shadow_global_read_pointer = !read_global;
                    program::Variable::ShadowType shadow_pointer_type = program::Variable::ShadowType::NONE;
                    if (needs_shadow_global_read_pointer)
                    {
                        if (epoch_type == graphlib::NodeEpochType::Forward)
                        {
                            shadow_pointer_type = any_consumers_cross_epoch(graph, q)
                                                      ? program::Variable::ShadowType::CROSS_PROGRAM
                                                      : program::Variable::ShadowType::CROSS_EPOCH;
                        }
                        else
                        {
                            shadow_pointer_type =
                                program::Variable::ShadowType::CROSS_EPOCH;  // backward & opt are always the last
                                                                             // consumer program
                        }
                    }
                    bool is_write_only = placer_solution.name_to_queue_placement[q->name()].write_only;
                    bool is_read_only = placer_solution.name_to_queue_placement[q->name()].read_only;
                    if (is_write_only)
                    {
                        int write_stride = placer_solution.name_to_queue_placement[q->name()].write_stride;
                        auto write_ptr = std::make_shared<program::Variable>("v_cache_write_index", true);
                        auto qs = program::QueueSettings(
                            q->name(),
                            program::RamAttributes{
                                .read_ptr_ = nullptr, .write_ptr_ = write_stride != 1 ? write_ptr : nullptr});
                        if (write_stride != 1)
                        {
                            qs.global_wrptr_autoinc =
                                placer_solution.name_to_queue_placement[q->name()]
                                    .write_stride;  // get t size of reader to support write pointer striding
                        }
                        qs.prologue = false;
                        has_cache_buffers = true;
                        queue_settings[epoch_index].push_back(qs);
                    }
                    else if (is_read_only)
                    {
                        // auto read_ptr = std::make_shared<program::Variable>("v_cache_write_index", true);
                        auto qs = program::QueueSettings(
                            q->name(), program::RamAttributes{.read_ptr_ = nullptr, .write_ptr_ = nullptr});

                        qs.read_only = true;
                        qs.global_rdptr_autoinc = 1;  // RAM needs global_rdptr_autoinc
                        queue_settings[epoch_index].push_back(qs);
                        has_cache_buffers = true;
                    }
                    else
                    {
                        const auto &[lptr, gptr] =
                            qvars(q, temporal_epoch_id, shadow_pointer_type, epoch_allocate == -1);

                        auto qs = program::QueueSettings(
                            q->name(),
                            program::QueueAttributes{
                                // see workaround above
                                //.read_ptr_global_= read_global ? gptr : nullptr,
                                .read_ptr_global_ = gptr,
                                .read_ptr_local_ = lptr,
                                .epoch_allocate = epoch_allocate,
                                .epoch_deallocate = epoch_deallocate,
                            });

                        bool rd_ptr_autoinc_enabled = (read_global && firmware_looping_enabled);
                        if (rd_ptr_autoinc_enabled)
                        {
                            qs.global_rdptr_autoinc = true;
                        }
                        queue_settings[epoch_index].push_back(qs);
                        std::uint32_t wrap_value = q->as<graphlib::QueueNode>()->get_num_entries();
                        increment_list[epoch_index].insert(std::make_tuple(lptr->name(), lptr, wrap_value, true));
                        // see workaround above
                        // if (read_global) {
                        increment_list[epoch_index].insert(std::make_tuple(gptr->name(), gptr, wrap_value, true));
                        //}
                    }
                }

                if ((epoch_type == graphlib::NodeEpochType::Forward) ||
                    (epoch_type == graphlib::NodeEpochType::Backward))
                {
                    // In forward and backward, all parameter and constant queues should be read in during prologue
                    for (graphlib::Node *q : parameter_queues[epoch_index])
                    {
                        // TODO(jchu): change RamAttributes (r/w ptrs) for repeated-structure integration
                        if (q->as<graphlib::InputNode>()->is_parameter())
                        {
                            if (placer_solution.name_to_queue_placement[q->name()].read_only)
                            {
                                auto read_ptr = std::make_shared<program::Variable>("v_cache_read", true);
                                auto qs = program::QueueSettings(
                                    q->name(), program::RamAttributes{.read_ptr_ = read_ptr, .write_ptr_ = nullptr});

                                qs.read_only = true;
                                qs.global_rdptr_autoinc = 1;  // RAM needs global_rdptr_autoinc
                                queue_settings[epoch_index].push_back(qs);
                                if (not has_cache_read_buffer)
                                {
                                    queue_variables.push_back(read_ptr);
                                    has_cache_read_buffer = true;
                                }
                            }
                            else
                            {
                                auto qs = program::QueueSettings(
                                    q->name(), program::RamAttributes{.read_ptr_ = nullptr, .write_ptr_ = nullptr});
                                qs.prologue = q->as<graphlib::InputNode>()->is_prologue();
                                queue_settings[epoch_index].push_back(qs);
                            }
                        }
                        else
                        {
                            auto qs = program::QueueSettings(
                                q->name(),
                                program::QueueAttributes{.read_ptr_global_ = nullptr, .read_ptr_local_ = nullptr});

                            qs.prologue = q->as<graphlib::InputNode>()->is_prologue();
                            if (!qs.prologue)
                                qs.rd_ptr_autoinc = false;
                            queue_settings[epoch_index].push_back(qs);
                        }
                    }
                }

                if (epoch_type == graphlib::NodeEpochType::Optimizer)
                {
                    // In optimizer, constants are read from dram
                    for (graphlib::Node *q : parameter_queues[epoch_index])
                    {
                        if (q->as<graphlib::InputNode>()->is_constant())
                        {
                            auto qs = program::QueueSettings(
                                q->name(),
                                program::QueueAttributes{.read_ptr_global_ = nullptr, .read_ptr_local_ = nullptr});

                            qs.prologue = false;
                            queue_settings[epoch_index].push_back(qs);
                        }
                    }
                }

                if (epoch_type == graphlib::NodeEpochType::Backward)
                {
                    // In backward, we need to epilogue copy them back to dram
                    for (graphlib::Node *q : gradient_queues[epoch_index])
                    {
                        auto qs = program::QueueSettings(
                            q->name(), program::RamAttributes{.read_ptr_ = nullptr, .write_ptr_ = nullptr});
                        qs.prologue = true;
                        qs.epilogue = true;
                        qs.zero = "$v_zero_grad";
                        queue_settings[epoch_index].push_back(qs);
                    }
                }

                if (epoch_type == graphlib::NodeEpochType::Optimizer)
                {
                    // In optimizer, we read & write parameter queues
                    for (graphlib::Node *q : parameter_queues[epoch_index])
                    {
                        if (q->as<graphlib::InputNode>()->is_parameter())
                        {
                            auto qs = program::QueueSettings(
                                q->name(), program::RamAttributes{.read_ptr_ = nullptr, .write_ptr_ = nullptr});
                            queue_settings[epoch_index].push_back(qs);
                        }
                        else
                        {
                            auto qs = program::QueueSettings(
                                q->name(),
                                program::QueueAttributes{.read_ptr_global_ = nullptr, .read_ptr_local_ = nullptr});
                        }
                    }

                    // Optimizer reads gradient accumulators
                    for (graphlib::Node *q : gradient_queues[epoch_index])
                    {
                        auto qs = program::QueueSettings(
                            q->name(), program::RamAttributes{.read_ptr_ = nullptr, .write_ptr_ = nullptr});
                        queue_settings[epoch_index].push_back(qs);
                    }
                }
            }

            std::string program_name = (epoch_type == graphlib::NodeEpochType::Forward)     ? "run_fwd_"
                                       : (epoch_type == graphlib::NodeEpochType::Backward)  ? "run_bwd_"
                                       : (epoch_type == graphlib::NodeEpochType::Optimizer) ? "run_opt_"
                                                                                            : "error";
            program_name += std::to_string(subgraph_index);

            for (const auto &[epoch, qvars] : qvars_map)
            {
                for (const auto &[type, qvars2] : qvars)
                {
                    for (const auto &[size, qvars3] : qvars2)
                    {
                        for (const auto &kv : qvars3)
                        {
                            queue_variables.push_back(std::get<0>(kv.second));  // lptr
                            queue_variables.push_back(std::get<1>(kv.second));  // gptr
                        }
                    }
                }
            }

            // Figure out allocate/deallocate commands
            std::unordered_map<std::uint32_t, std::shared_ptr<program::AllocateQueue>> alloc_queue_cmds;
            std::unordered_map<std::uint32_t, std::shared_ptr<program::DeallocateQueue>> dealloc_queue_cmds;
            std::unordered_map<std::uint32_t, std::vector<program::QueueSettings>> to_alloc;
            std::unordered_map<std::uint32_t, std::vector<program::QueueSettings>> to_dealloc;
            std::unordered_set<std::string> visited_queue_alloc_dealloc;
            for (std::uint32_t epoch_index = 0; epoch_index < epochs.size(); epoch_index++)
            {
                std::uint32_t epoch = epochs[epoch_index];
                if (placer_solution.epoch_id_to_subgraph_index[epoch] != subgraph_index)
                    continue;
                for (const program::QueueSettings &qs : queue_settings[epoch_index])
                {
                    try
                    {
                        // queue should have single alloc/dealloc invocation: {earliest alloc, latest dealloc}
                        if (visited_queue_alloc_dealloc.find(qs.name()) == visited_queue_alloc_dealloc.end())
                        {
                            int epoch_allocate = qs.epoch_allocate();
                            if (epoch_allocate >= 0)
                                to_alloc[epoch_allocate].push_back(qs);

                            int epoch_deallocate = qs.epoch_deallocate();
                            if (epoch_deallocate >= 0)
                                to_dealloc[epoch_deallocate].push_back(qs);
                            visited_queue_alloc_dealloc.insert(qs.name());
                        }
                    }
                    catch (std::out_of_range &e)
                    {
                        throw std::runtime_error("Invalid allocate/deallocate epoch for " + qs.name());
                    }
                }
            }
            for (auto &[epoch_index, qs] : to_alloc)
                alloc_queue_cmds.emplace(std::make_pair(epoch_index, std::make_shared<program::AllocateQueue>(qs)));
            for (auto &[epoch_index, qs] : to_dealloc)
                dealloc_queue_cmds.emplace(std::make_pair(epoch_index, std::make_shared<program::DeallocateQueue>(qs)));

            bool has_zero_grad = epoch_type == graphlib::NodeEpochType::Backward;
            bool is_optimizer_loop = epoch_type == graphlib::NodeEpochType::Optimizer;
            std::vector<std::vector<std::uint32_t>> temporal_to_spatial_epochs =
                get_temporal_to_concurrent_spatial_epochs(placer_solution, epochs);

            programs.push_back(program::Program::loop_template(
                program_name,
                queue_variables,
                graph->get_microbatch(),
                has_zero_grad,
                is_optimizer_loop,
                has_cache_buffers,
                [&queue_settings,
                 &temporal_to_spatial_epochs,
                 epoch_type,
                 &increment_list,
                 has_zero_grad,
                 &epoch_to_epoch_index,
                 &arch_string,
                 &last_bwd_epoch,
                 &alloc_queue_cmds,
                 &dealloc_queue_cmds,
                 &buda_graph,
                 is_optimizer_loop,
                 &firmware_looping_enabled,
                 subgraph_index,
                 placer_solution](program::Program &p)
                {
                    // Loop body
                    for (std::uint32_t temporal_epoch_id = 0; temporal_epoch_id < temporal_to_spatial_epochs.size();
                         temporal_epoch_id++)
                    {
                        // Add queue allocate command for the temporal epoch
                        if (alloc_queue_cmds.count(temporal_epoch_id) > 0)
                            p.add(alloc_queue_cmds.at(temporal_epoch_id));

                        std::vector<std::shared_ptr<program::DeallocateQueue>> queued_dealloc_cmds;

                        bool empty_temporal_epoch = true;
                        for (std::uint32_t spatial_epoch_index = 0;
                             spatial_epoch_index < temporal_to_spatial_epochs[temporal_epoch_id].size();
                             spatial_epoch_index++)
                        {
                            std::uint32_t epoch = temporal_to_spatial_epochs[temporal_epoch_id][spatial_epoch_index];
                            empty_temporal_epoch &= buda_graph.ops[epoch].empty();
                        }

                        for (std::uint32_t spatial_epoch_index = 0;
                             spatial_epoch_index < temporal_to_spatial_epochs[temporal_epoch_id].size();
                             spatial_epoch_index++)
                        {
                            if (empty_temporal_epoch)
                            {
                                continue;
                            }
                            std::uint32_t epoch = temporal_to_spatial_epochs[temporal_epoch_id][spatial_epoch_index];
                            if (placer_solution.epoch_id_to_subgraph_index.at(epoch) != subgraph_index)
                                continue;
                            std::uint32_t epoch_index = epoch_to_epoch_index.at(epoch);

                            const std::string subgraph_name =
                                get_subgraph_name(epoch_type, epoch, arch_string, temporal_epoch_id, subgraph_index);

                            // Generate execute command
                            if (empty_temporal_epoch and buda_graph.ops[epoch].size() == 0)
                            {
                                continue;
                            }
                            p.add(std::make_shared<program::Execute>(subgraph_name, queue_settings[epoch_index]));
                        }

                        // Write out all dealloc commands
                        if (dealloc_queue_cmds.count(temporal_epoch_id) > 0)
                            p.add(dealloc_queue_cmds.at(temporal_epoch_id));

                        std::unordered_set<std::string> q_ptr_variables_incremented_this_temporal_epoch = {};

                        for (std::uint32_t spatial_epoch_index = 0;
                             spatial_epoch_index < temporal_to_spatial_epochs[temporal_epoch_id].size();
                             spatial_epoch_index++)
                        {
                            std::uint32_t epoch = temporal_to_spatial_epochs[temporal_epoch_id][spatial_epoch_index];
                            if (placer_solution.epoch_id_to_subgraph_index.at(epoch) != subgraph_index)
                                continue;
                            std::uint32_t epoch_index = epoch_to_epoch_index.at(epoch);

                            for (auto [var_name, var, wrap_value, double_wrap] : increment_list[epoch_index])
                            {
                                if (firmware_looping_enabled)
                                {
                                    continue;
                                }
                                if (var->needs_shadow_global_read_pointer())
                                {
                                    var = var->get_shadow_global_read_pointer();
                                }
                                if (q_ptr_variables_incremented_this_temporal_epoch.find(var->name()) ==
                                    q_ptr_variables_incremented_this_temporal_epoch.end())
                                {
                                    int wrap =
                                        double_wrap ? wrap_value * 2 : wrap_value;  // hack for backend to set wrap to
                                                                                    // wrap x2 to match hardware wrap
                                    p.instruction_incwrap(
                                        var,
                                        is_optimizer_loop or not double_wrap ? p.get_var("c_one")
                                                                             : p.get_var("c_microbatch_size"),
                                        wrap);
                                    q_ptr_variables_incremented_this_temporal_epoch.insert(var->name());
                                }
                            }

                            // Clear zero grad for subsequent iterations of the loop
                            if (has_zero_grad && (epoch == last_bwd_epoch))
                            {
                                program::VariableP v_zero_grad = p.get_var("v_zero_grad");
                                p.set_variable_value(v_zero_grad, 0);
                            }
                        }
                    }
                }));
        }
    }

    return programs;
}

static std::vector<std::size_t> get_input_dram_io_buf_size_tiles(
    Graph const *graph,
    DeviceConfig const &device_config,
    placer::PlacerSolution const &placer_solution,
    Node const *node,
    balancer::OpModel const &op_model)
{
    placer::OpPlacement const &op_placement = placer_solution.name_to_op_placement.at(node->name());
    auto dram_io_queue_node = [&placer_solution, &op_placement](graphlib::Node *operand)
    {
        auto *queue = dynamic_cast<graphlib::QueueNode *>(operand);
        if (not queue)
            return false;
        placer::QueuePlacement const &queue_placement = placer_solution.name_to_queue_placement.at(queue->name());

        // Currently router (part of net2pipe that does chip2chip routing) does not support input_dram_io_buf_size_tiles
        // and its presence in the netlist actually causes issues.  So we need to disable emitting this attribute for
        // remote dram reads. For more context:
        //   tenstorrent/budabackend#1979#note_263659
        bool remote_read = queue_placement.chip_id != op_placement.chip_id;
        return not remote_read and (queue->is_input() or queue->is_epoch_to_epoch() or queue->is_buffering());
    };

    std::size_t num_dram_readers = 0;
    auto operands = graph->data_operands(node);
    std::vector<std::size_t> input_dram_io_buf_size_tiles(operands.size(), 0);
    int input_idx = 0;
    for (Node *operand : operands)
    {
        bool is_prologue = bool(op_model.parameter_buffers[input_idx]);
        num_dram_readers += int(dram_io_queue_node(operand) and not is_prologue);
        ++input_idx;
    }

    if (num_dram_readers == 0 or env_as<bool>("PYBUDA_DISABLE_EXPLICIT_DRAM_IO"))
        return input_dram_io_buf_size_tiles;

    // DRAM IO buffer sizing
    TT_ASSERT(op_model.get_l1_memory_usage() <= device_config.get_l1_usable_size());

    // We can reclaim the default pipegen carve out space for DRAM io get_l1_dram_io_backend_reserved_size
    std::size_t l1_per_input_dram_prefetch_buffer_size =
        device_config.get_l1_dram_io_backend_reserved_size() / num_dram_readers;

    input_idx = 0;
    for (Node *operand : operands)
    {
        bool is_prologue = bool(op_model.parameter_buffers[input_idx]);
        if (dram_io_queue_node(operand) and not is_prologue)
        {
            std::size_t input_buffer_bytes = op_model.input_buffers[input_idx].single_buffered_size_bytes();
            std::size_t multiplier = l1_per_input_dram_prefetch_buffer_size / input_buffer_bytes;

            if (multiplier > 0)
            {
                // If we can fit > 0 additional multiplier of the input buffer,
                // then we try to fit some multiple of the input buffer
                std::size_t input_buffer_tiles = op_model.input_buffers[input_idx].single_buffered_size_tiles();
                input_dram_io_buf_size_tiles[input_idx] = input_buffer_tiles * multiplier;
            }
            else
            {
                // If we can't fit any multiplier input buffers, then we just
                // allocate as many tiles as we can into this region
                std::size_t input_buffer_tiles =
                    l1_per_input_dram_prefetch_buffer_size /
                    balancer::tile_size_bytes(op_model.input_buffers[input_idx].data_format);
                input_dram_io_buf_size_tiles[input_idx] = input_buffer_tiles;
            }
        }
        ++input_idx;
    }

    return input_dram_io_buf_size_tiles;
}

// Create Buda queues, program, and graphs
BudaNetlist lower_to_buda_netlist(
    Graph *graph,
    std::string &graph_name,
    placer::PlacerSolution &placer_solution,
    std::shared_ptr<balancer::BalancerSolution> balancer_solution,
    const std::vector<std::uint32_t> &chip_ids,
    const DeviceConfig &device_config,
    bool enable_forked_dram_inputs)
{
    BudaNetlist net;
    const std::string &arch_string = device_config.arch_name;
    net.graphs.push_back(BudaGraph(graph_name, arch_string, graph->get_microbatch()));
    BudaGraph &buda_graph = net.graphs.back();
    std::unordered_map<int, BudaOperand> node_to_op;
    std::unordered_set<std::uint32_t> fused_ops_register;

    std::uint32_t epoch_count = balancer_solution->placer_solution.epoch_id_to_epoch_info.size();
    buda_graph.epoch_types.resize(epoch_count);
    buda_graph.ops.resize(epoch_count);
    for (auto &[epoch_id, epoch_info] : balancer_solution->placer_solution.epoch_id_to_epoch_info)
    {
        buda_graph.epoch_types[epoch_id] = epoch_info.epoch_type;
    }

    // Returns a mapping of keys that are input edges that can reuse the DRAM reads of the mapped values
    std::unordered_map<graphlib::Edge, graphlib::Edge> forked_dram_inputs = tt::passes::get_forked_dram_inputs(
        enable_forked_dram_inputs, graph, &placer_solution.name_to_op_placement, &balancer_solution->op_models);

    for (Node *node : graphlib::topological_sort(*graph))
    {
        try
        {
            if (node->node_type() == NodeType::kInput or node->node_type() == NodeType::kOutput or
                node->node_type() == NodeType::kQueue)
            {
                BudaQueue q = create_queue(
                    graph,
                    node->as<graphlib::QueueNode>(),
                    placer_solution.name_to_queue_placement.at(node->name()),
                    balancer_solution->block_shapes);
                net.queues.push_back(q);
                node_to_op.insert(std::make_pair(node->id(), q.name));
            }
            else if (node->node_type() == NodeType::kBudaOp)
            {
                std::vector<BudaOperand> operands;
                std::vector<tt::DataFormat> input_df;
                for (Node *in_node : graph->data_operands(node))
                {
                    operands.push_back(node_to_op.at(in_node->id()));
                    input_df.push_back(graph->node_by_id(in_node->id())->output_df());
                }

                std::vector<graphlib::Edge> forked_dram_edges;

                for (auto operand : graph->operand_data_edges(node))
                {
                    if (forked_dram_inputs.find(operand) != forked_dram_inputs.end())
                    {
                        forked_dram_edges.push_back(forked_dram_inputs[operand]);
                    }
                }

                balancer::OpModel const &op_model = balancer_solution->op_models.at(node->name());
                balancer::BlockShape const &block_shape = balancer_solution->block_shapes.at(node->name());
                placer::OpPlacement placement = placer_solution.name_to_op_placement.at(node->name());
                BudaOpAttrs buda_attrs = node->as<graphlib::BudaOpNode>()->op_type().buda_attrs;
                bool ignore_tms = false;
                std::vector<std::size_t> input_dram_io_buf_size_tiles =
                    get_input_dram_io_buf_size_tiles(graph, device_config, placer_solution, node, op_model);

                if (op_model.has_sparse_buffer())
                {
                    BudaOp sparse_buffer_op;
                    std::tie(sparse_buffer_op, operands, input_df, buda_attrs, placement) = create_sparse_buffer_op(
                        graph,
                        node->as<graphlib::BudaOpNode>(),
                        operands,
                        input_df,
                        buda_attrs,
                        placement,
                        placer_solution.name_to_queue_placement,
                        op_model,
                        block_shape,
                        arch_string,
                        input_dram_io_buf_size_tiles);

                    // Sparse buffer handled the incoming tms
                    ignore_tms = true;
                    buda_graph.ops[placement.epoch_id()].push_back(sparse_buffer_op);
                }

                BudaOp op = create_op(
                    graph,
                    node->as<graphlib::BudaOpNode>(),
                    operands,
                    input_df,
                    buda_attrs,
                    placement,
                    placer_solution.name_to_queue_placement,
                    op_model,
                    block_shape,
                    arch_string,
                    input_dram_io_buf_size_tiles,
                    forked_dram_edges,
                    ignore_tms);

                if (node->as<graphlib::BudaOpNode>()->is_fused_op())
                {
                    BudaFusedOp fused_op = create_fused_op(node->as<graphlib::BudaOpNode>(), op_model);
                    bool reused = false;
                    for (const BudaFusedOp &prev : net.fused_ops)
                    {
                        if (prev.equivalent(fused_op))
                        {
                            // We can reuse an old op
                            op.attrs["fused_op_id"] = (int)prev.id;
                            reused = true;
                            break;
                        }
                    }
                    if (!reused)
                    {
                        net.fused_ops.push_back(fused_op);
                        TT_ASSERT(fused_ops_register.count(fused_op.id) == 0, "Duplicate fused op id found!");
                        fused_ops_register.insert(fused_op.id);
                    }

                    if (env_as<bool>("PYBUDA_EXP_APPROX"))  // TODO: config
                        op.attrs["approximate_mode"] = true;
                }

                buda_graph.ops[placement.epoch_id()].push_back(op);

                node_to_op.insert(std::make_pair(node->id(), op.name));
            }
            else if (node->node_type() == NodeType::kBudaNaryTM)
            {
                std::vector<BudaOperand> operands;
                for (Node *in_node : graph->data_operands(node)) operands.push_back(node_to_op.at(in_node->id()));
                BudaNaryTM tm = create_nary_tm(node->as<graphlib::BudaNaryTMNode>(), operands);
                node_to_op.insert(std::make_pair(node->id(), tm));
            }
        }
        catch (std::out_of_range &e)
        {
            throw std::runtime_error(
                "Op or queue missing in placement results for " + node->name() + ", something went wrong.");
        }
    }

    for (size_t epoch_id = 0; epoch_id < buda_graph.epoch_types.size(); ++epoch_id)
    {
        int chip_id = placer_solution.epoch_id_to_chip.at(epoch_id);
        buda_graph.epoch_target_devices.push_back(BudaDevice(chip_id));
        buda_graph.epoch_to_temporal_epoch_id.push_back(placer_solution.temporal_epoch_id(epoch_id));
        buda_graph.epoch_to_subgraph_index.push_back(placer_solution.epoch_id_to_subgraph_index[epoch_id]);
    }

    net.programs = create_programs(graph, placer_solution, buda_graph, arch_string);
    net.chip_ids = chip_ids;
    net.arch_string = arch_string;

    std::stringstream ss;
    to_debug_info(ss, device_config);
    net.debug_info = Comment(ss);

    return net;
}

BudaNetlist merge_netlists(std::vector<BudaNetlist> subgraphs)
{
    BudaNetlist net;
    net.arch_string = subgraphs[0].arch_string;
    net.chip_ids = subgraphs[0].chip_ids;
    for (auto subgraph : subgraphs)
    {
        for (auto fused_op : subgraph.fused_ops)
        {
            net.fused_ops.push_back(fused_op);
        }
        for (auto program : subgraph.programs)
        {
            net.programs.push_back(program);
        }
        for (auto queue : subgraph.queues)
        {
            net.queues.push_back(queue);
        }
        for (auto graph : subgraph.graphs)
        {
            net.graphs.push_back(graph);
        }
    }
    return net;
}
}  // namespace tt
