// The game's shaders, translated offline to SPIR-V (tools/nvn_shaders.py: work/nvn_shaders/<key>.spv) with
// their reflection (tools/nvn_refl.py: refl.tsv), found by the NVN program stage key (nvn::prog_key).
//
// Each translated shader keeps all its resources in descriptor set 0, numbered from binding 0 (the
// translator's support buffer) up. Here each stage gets a set of its own (Stage::set), so the stages of
// one pipeline never collide: the module's DescriptorSet decorations are rewritten at load.
#pragma once
#include <vector>

#include "vkctx.h"

namespace nvk {

// NVN stage slots as the interface layer numbers them (cmdbuf.cpp st_slot): task and mesh take the
// tessellation slots.
enum ShStage : u8 { SH_VS = 0, SH_PS = 1, SH_GS = 2, SH_TS = 3, SH_MS = 4, SH_CS = 5, SH_N = 6 };

// What a binding is in NVN terms (refl.tsv kinds).
enum class BKind : u8 {
    Support,  // the translator's support buffer (binding 0)
    Ubo,      // uniform slot reg
    C1,       // the shader's constant segment
    Drv0,     // driver segment c[0]
    Drv2,     // driver segment c[2]
    Push,     // push constants (c[24])
    Ssbo,     // storage slot reg of the stage
    SsboUbo,  // storage buffer at the address in uniform slot reg, byte off
    UboPtr,   // compute: uniform slot reg as a storage buffer
    GmemMap,  // global memory pool table
    GmemLog,  // global memory write log
    Tex,      // texture slot reg (combined handle)
    Img,      // image slot reg
    Unknown,
};

struct Binding {
    u32 binding = 0;
    BKind kind = BKind::Unknown;
    u32 reg = 0, off = 0, aux = 0;  // aux: sampler type (Tex / Img), written (Ssbo...)
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    u32 count = 1;
    u8 dim = 1;                     // SPIR-V Dim of an image binding (0 1D, 1 2D, 2 3D, 3 Cube, 5 Buffer)
    bool arrayed = false, ms = false;
};

struct Shader {
    u64 key = 0;
    char name[17] = {};
    ShStage stage = SH_VS;
    u32 set = 0;                    // its descriptor set in a pipeline layout
    VkShaderStageFlagBits vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    std::vector<Binding> bindings;
    u32 kinds = 0;                  // bit 1 << BKind of each kind among its bindings
    std::vector<u8> c1;
    u32 inputs = 0;                 // vertex stage: input locations read
    u8 input_type[32] = {};         // 0 float, 1 sint, 2 uint
    u32 outputs = 0;                // pixel stage: colour locations written
    u32 local[3] = {1, 1, 1};
    const char* unsupported = nullptr;  // why draws with it are skipped
};

// The shader with this key (prog_key bytes), loaded on first use; null when no translation exists
// (logged once per key). Thread: the submission thread.
const Shader* shader(const u8 key[8]);
// The same by the key as a number (Shader::key). Any thread.
const Shader* shader_by_key(u64 key);
// The folder of the translated shaders (work/nvn_shaders, or HOST_NVN_SHADERS).
std::string shader_folder();
// Number of shaders in the reflection table (0: table missing).
size_t shader_count();

// Set index of a stage: vertex 0, pixel 1, geometry 2, task 3, mesh 4; compute 0.
u32 stage_set(ShStage s);

}  // namespace nvk
