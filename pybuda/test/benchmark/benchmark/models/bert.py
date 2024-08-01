# SPDX-FileCopyrightText: © 2024 Tenstorrent AI ULC

# SPDX-License-Identifier: Apache-2.0
import os
import math
import pybuda
import torch
from typing import Optional

from pybuda import PyTorchModule
from pybuda.config import _get_global_compiler_config
from transformers import BertModel, BertConfig

from ..common import benchmark_model

# Embedding wrapper that extends and passes attention mask through - to run on host
class EmbWrapper(torch.nn.Module):
    def __init__(self, bert):
        super().__init__()
        self.bert = bert
    def forward(self, input_ids, attention_mask, token_type_ids):
        attention_mask = attention_mask * 1.0
        emb_output = self.bert.embeddings(input_ids, token_type_ids)
        # input_ids = input_ids * 1.0
        # emb_output = input_ids.unsqueeze(2).repeat(1, 1, 1024)
        extended_attention_mask = (1.0 - attention_mask) * -10000.0
        extended_attention_mask = extended_attention_mask.unsqueeze(dim=-2)
        return emb_output, extended_attention_mask

class BertWrapper(torch.nn.Module):
    def __init__(self, bert):
        super().__init__()
        self.bert = bert

    def forward(self, input_ids, attention_mask, token_type_ids):
        attention_mask = attention_mask * 1.0
        emb_output = self.bert.embeddings(input_ids, token_type_ids)
        # input_ids = input_ids * 1.0
        # emb_output = input_ids.unsqueeze(2).repeat(1, 1, 1024)
        extended_attention_mask = (1.0 - attention_mask) * -10000.0
        extended_attention_mask = extended_attention_mask.unsqueeze(dim=-2)
        return self.bert.encoder(emb_output, extended_attention_mask)

@benchmark_model(configs=["tiny", "base", "large", "base_tc", "large_tc"])
def bert(training: bool, config: str, microbatch: int, devtype: str, arch: str, data_type: str, force_num_layers: Optional[int] = None):

    compiler_cfg = _get_global_compiler_config()
    os.environ["PYBUDA_DISABLE_DYNAMIC_DRAM"] = "1"

    if config == "tiny":
        model_name = "prajjwal1/bert-tiny"
        seq_len = 128
        target_microbatch = 512
        compiler_cfg.enable_auto_transposing_placement = True
        os.environ["PYBUDA_EXP_APPROX"] = "1" 
    elif config == "base":
        model_name = "bert-base-uncased"
        seq_len = 128
        target_microbatch = 128
        # Testing!
        #if not force_num_layers:
        #    pybuda.config._get_global_compiler_config().place_on_new_epoch(f"matmul_518")
    elif config == "large":
        model_name = "bert-large-uncased"
        if training:
            seq_len=128
            target_microbatch = 32
        else:
            seq_len = 384
            target_microbatch = 128

        # start each epoch at the beginning of the module
        if "PYBUDA_MODULAR_BERT" in os.environ:
            layers = force_num_layers if force_num_layers else 24
            for i in range(layers):
                pybuda.config._get_global_compiler_config().place_on_new_epoch(f"matmul_{2+53*i}")

        elif "PYBUDA_MULTICHIP_BERT" in os.environ:
            layers = force_num_layers if force_num_layers else 24
            chips = int(os.environ["PYBUDA_MULTICHIP_BERT"])
            chip_breaks = []
            for i in range(chips):
                chip_breaks.append(i*(math.ceil(layers/chips)))
            for i in range(layers):
                if i in chip_breaks:
                    pybuda.config._get_global_compiler_config().place_on_new_chip(f"matmul_{2+53*i}")
                else:
                    pybuda.config._get_global_compiler_config().place_on_new_epoch(f"matmul_{2+53*i}")

        else:
            # Trying to avoid 4x output bw - manual for now
            layers = force_num_layers if force_num_layers else 24
            for i in range(layers):
                if (i%2 == 1):
                    pybuda.config._get_global_compiler_config().place_on_new_epoch(f"matmul_{41+53*i}")
    elif config == "base_tc":
        model_name = "textattack/bert-base-uncased-SST-2"
        seq_len = 128
        target_microbatch = 128
    elif config == "large_tc":
        model_name = "assemblyai/bert-large-uncased-sst2"
        seq_len = 384
        target_microbatch = 128
        compiler_cfg.enable_auto_transposing_placement = True
        if compiler_cfg.balancer_policy == "default":
            compiler_cfg.balancer_policy = "Ribbon"
            os.environ["PYBUDA_RIBBON2"] = "1"
            os.environ["PYBUDA_TEMP_ENABLE_NEW_FUSED_ESTIMATES"] = "1"
            os.environ["PYBUDA_TEMP_ENABLE_NEW_SPARSE_ESTIMATES"] = "1"
            os.environ["PYBUDA_TEMP_SCALE_SPARSE_ESTIMATE_ARGS"] = "1"
            os.environ["PYBUDA_RIBBON2_CALCULATE_TARGET_CYCLES"] = "1"
            if data_type == "Bfp8_b":
                os.environ["PYBUDA_EXP_APPROX"] = "1"
                pybuda.config.configure_mixed_precision(op_type="add", output_df=pybuda.DataFormat.Float16_b)
                pybuda.config.configure_mixed_precision(op_type="subtract", output_df=pybuda.DataFormat.Float16_b)
                pybuda.config.configure_mixed_precision(op_type="reciprocal", output_df=pybuda.DataFormat.Float16_b)
            if data_type == "Fp16_b":
                os.environ["PYBUDA_TEMP_DISABLE_MODEL_KB_PROLOGUE_BW"] = "1"
    else:
        raise RuntimeError("Unknown config")

    if microbatch == 0:
        microbatch = target_microbatch

    cfg = BertConfig.from_pretrained(model_name)
    if force_num_layers:
        cfg.num_hidden_layers = force_num_layers
    model = BertModel(config=cfg)
    if not training:
        model.eval()

    #
    # Apply functional workarounds
    #
    if training:
        #_get_global_compiler_config().enable_broadcast_splitting = True # fork error workaround
        pass

    #
    # Create inputs, targets, models
    #
    if "PYBUDA_SKIP_EMBEDDINGS" in os.environ:
        inputs = [
            torch.rand(microbatch, seq_len, cfg.hidden_size), # emb_output
            torch.rand(microbatch, 1, 1, seq_len), # extended_attention_mask
        ]
        models = {"tt": PyTorchModule("bert_encoders", model.encoder)}
    elif bool(int(os.environ.get("PYBUDA_DEVICE_EMBEDDINGS", "0"))) and not training:
        inputs = [
            torch.randint(high=25000, size=(microbatch, seq_len), dtype=torch.int), # input tokens
            torch.randint(high=2, size=(microbatch, 1, seq_len), dtype=torch.float), # mask
            torch.randint(high=2, size=(microbatch, seq_len), dtype=torch.int), # token type IDs
        ]
        models = {"tt": PyTorchModule("bert", BertWrapper(model))}
        pybuda.config._get_global_compiler_config().cpu_fallback_ops.remove("embedding")
    else:
        inputs = [
            torch.randint(high=25000, size=(microbatch, seq_len), dtype=torch.int), # input tokens
            torch.randint(high=2, size=(microbatch, seq_len), dtype=torch.int), # mask
            torch.randint(high=2, size=(microbatch, seq_len), dtype=torch.int), # token type IDs
        ]
        models = {"cpu-pre": PyTorchModule("bert_emb", EmbWrapper(model)), "tt": PyTorchModule("bert_encoders", model.encoder)}

    targets = tuple()
    if training:
        targets = [torch.rand(microbatch, seq_len, cfg.hidden_size)]
        models["cpu-loss"] = PyTorchModule("l1loss", torch.nn.L1Loss())

    #pybuda.config.set_num_repeated_patterns(models["tt"].get_name(), len(models["tt"].module.layer))

    return models, inputs, targets, {}


