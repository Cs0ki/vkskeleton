#!/usr/bin/env python3
"""Generate two minimal SPIR-V 1.0 modules for vkskeleton.c.

The modules are emitted directly, so no shader compiler is required at build
time.  The vertex shader consumes CPU-transformed vec3 positions (including
Vulkan depth), UVs, a vec4 color and per-vertex alpha-test parameters; GoldSrc
fixed-function matrix work stays on the CPU side.  The fragment shader
multiplies the color by a sampled texture and emulates GL_ALPHA_TEST.
"""
from pathlib import Path
import struct
import sys

MAGIC = 0x07230203
VERSION = 0x00010000

# Opcode names used below (SPIR-V 1.0).
OP_CAPABILITY = 17
OP_MEMORY_MODEL = 14
OP_ENTRY_POINT = 15
OP_EXECUTION_MODE = 16
OP_TYPE_VOID = 19
OP_TYPE_BOOL = 20
OP_TYPE_FLOAT = 22
OP_TYPE_VECTOR = 23
OP_TYPE_IMAGE = 25
OP_TYPE_SAMPLED_IMAGE = 27
OP_TYPE_FUNCTION = 33
OP_TYPE_POINTER = 32
OP_CONSTANT = 43
OP_CONSTANT_FALSE = 42
OP_VARIABLE = 59
OP_FUNCTION = 54
OP_LABEL = 248
OP_LOAD = 61
OP_STORE = 62
OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87
OP_FMUL = 133
OP_FORD_EQUAL = 180
OP_FORD_NOT_EQUAL = 182
OP_FORD_LESS_THAN = 184
OP_FORD_GREATER_THAN = 186
OP_FORD_LESS_THAN_EQUAL = 188
OP_FORD_GREATER_THAN_EQUAL = 190
OP_LOGICAL_AND = 167
OP_LOGICAL_OR = 166
OP_COMPOSITE_CONSTRUCT = 80
OP_COMPOSITE_EXTRACT = 81
OP_RETURN = 253
OP_FUNCTION_END = 56
OP_DECORATE = 71
OP_SELECTION_MERGE = 247
OP_BRANCH = 249
OP_BRANCH_CONDITIONAL = 250
OP_KILL = 252

OP_TYPE_INT = 21
OP_TYPE_ARRAY = 28
OP_DOT = 148
BUILTIN_CLIP_DISTANCE = 3
OP_TYPE_MATRIX = 24
OP_TYPE_STRUCT = 30
OP_ACCESS_CHAIN = 65
OP_MEMBER_DECORATE = 72
OP_MATRIX_TIMES_VECTOR = 145
SC_PUSH_CONSTANT = 9
DEC_BLOCK = 2
DEC_COL_MAJOR = 5
DEC_MATRIX_STRIDE = 7
DEC_OFFSET = 35
SC_UNIFORM_CONSTANT = 0
SC_INPUT = 1
SC_OUTPUT = 3
SC_GLSL450 = 1
EXEC_VERTEX = 0
EXEC_FRAGMENT = 4
DEC_LOCATION = 30
DEC_BINDING = 33
DEC_DESCRIPTOR_SET = 34
DEC_BUILTIN = 11
BUILTIN_POSITION = 0
MODE_ORIGIN_UPPER_LEFT = 7

# GL alpha-test function enumerants as passed through vk_vertex_t.
GL_NEVER = 512
GL_LESS = 513
GL_EQUAL = 514
GL_LEQUAL = 515
GL_GREATER = 516
GL_NOTEQUAL = 517
GL_GEQUAL = 518


def fbits(value):
    return struct.unpack("<I", struct.pack("<f", float(value)))[0]


def string_words(text):
    data = text.encode("utf-8") + b"\0"
    while len(data) % 4:
        data += b"\0"
    return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]


class Module:
    def __init__(self, bound):
        self.words = [MAGIC, VERSION, 0, bound, 0]

    def inst(self, op, *operands):
        self.words.append(((len(operands) + 1) << 16) | op)
        self.words.extend(operands)


class Ids:
    def __init__(self, start):
        self.next = start

    def take(self, count=1):
        first = self.next
        self.next += count
        return first if count == 1 else list(range(first, self.next))


def vertex_module():
    """Fixed-function transform on the GPU.

    in  vec3 pos (location 0): untransformed GL vertex
    push_constant { mat4 mvp; }: GL modelview*projection already combined
        with the GL->Vulkan fixups (Y flip, depth [-w,w] -> glDepthRange
        mapped into [0,w]) on the CPU, once per draw
    gl_Position = mvp * vec4(pos, 1); the GPU clips and divides.
    """
    ids = Ids(1)
    void = ids.take(); flt = ids.take(); i32 = ids.take()
    vec2 = ids.take(); vec3 = ids.take(); vec4 = ids.take(); mat4 = ids.take()
    fn_type = ids.take()
    pc_struct = ids.take(); ptr_pc = ids.take(); ptr_pc_mat = ids.take()
    ptr_in3 = ids.take(); ptr_in2 = ids.take(); ptr_in4 = ids.take()
    ptr_out2 = ids.take(); ptr_out4 = ids.take()
    const1 = ids.take(); const_i0 = ids.take()
    in_pos = ids.take(); in_uv = ids.take(); in_params = ids.take()
    in_color = ids.take(); in_uv2 = ids.take()
    v_uv = ids.take(); v_color = ids.take(); v_params = ids.take()
    v_uv2 = ids.take(); gl_pos = ids.take(); pc = ids.take()
    fn_main = ids.take(); label = ids.take()
    pos_val = ids.take(); x_val = ids.take(); y_val = ids.take()
    z_val = ids.take(); pos4 = ids.take(); mat_ptr = ids.take()
    mat_val = ids.take(); clip_val = ids.take()
    uv_val = ids.take(); color_val = ids.take(); params_val = ids.take()
    uv2_val = ids.take()
    # glClipPlane emulation: push_constant.clip (object-space plane for this
    # draw; (0,0,0,1) when disabled) -> gl_ClipDistance[0].
    u32 = ids.take(); const_u1 = ids.take(); const_i1 = ids.take()
    arr_f1 = ids.take(); ptr_out_arr = ids.take(); ptr_out_f = ids.take()
    ptr_pc_vec4 = ids.take(); clip_dist = ids.take()
    clip_ptr = ids.take(); clip_vec = ids.take(); dist_val = ids.take()
    dist_ptr = ids.take()

    m = Module(ids.next)
    m.inst(OP_CAPABILITY, 1)                     # Shader
    m.inst(OP_CAPABILITY, 32)                    # ClipDistance
    m.inst(OP_MEMORY_MODEL, 0, SC_GLSL450)       # Logical
    m.inst(OP_ENTRY_POINT, EXEC_VERTEX, fn_main, *string_words("main"),
           in_pos, in_uv, in_params, in_color, in_uv2,
           v_uv, v_color, v_params, v_uv2, gl_pos, clip_dist)
    m.inst(OP_DECORATE, in_pos, DEC_LOCATION, 0)
    m.inst(OP_DECORATE, in_uv, DEC_LOCATION, 1)
    m.inst(OP_DECORATE, in_color, DEC_LOCATION, 2)
    m.inst(OP_DECORATE, in_params, DEC_LOCATION, 3)
    m.inst(OP_DECORATE, in_uv2, DEC_LOCATION, 4)
    m.inst(OP_DECORATE, v_uv, DEC_LOCATION, 0)
    m.inst(OP_DECORATE, v_color, DEC_LOCATION, 1)
    m.inst(OP_DECORATE, v_params, DEC_LOCATION, 2)
    m.inst(OP_DECORATE, v_uv2, DEC_LOCATION, 3)
    m.inst(OP_DECORATE, gl_pos, DEC_BUILTIN, BUILTIN_POSITION)
    m.inst(OP_DECORATE, pc_struct, DEC_BLOCK)
    m.inst(OP_MEMBER_DECORATE, pc_struct, 0, DEC_COL_MAJOR)
    m.inst(OP_MEMBER_DECORATE, pc_struct, 0, DEC_OFFSET, 0)
    m.inst(OP_MEMBER_DECORATE, pc_struct, 0, DEC_MATRIX_STRIDE, 16)
    m.inst(OP_MEMBER_DECORATE, pc_struct, 1, DEC_OFFSET, 64)
    m.inst(OP_DECORATE, clip_dist, DEC_BUILTIN, BUILTIN_CLIP_DISTANCE)

    m.inst(OP_TYPE_VOID, void)
    m.inst(OP_TYPE_FLOAT, flt, 32)
    m.inst(OP_TYPE_INT, i32, 32, 1)
    m.inst(OP_TYPE_INT, u32, 32, 0)
    m.inst(OP_TYPE_VECTOR, vec2, flt, 2)
    m.inst(OP_TYPE_VECTOR, vec3, flt, 3)
    m.inst(OP_TYPE_VECTOR, vec4, flt, 4)
    m.inst(OP_TYPE_MATRIX, mat4, vec4, 4)
    m.inst(OP_TYPE_FUNCTION, fn_type, void)
    m.inst(OP_TYPE_STRUCT, pc_struct, mat4, vec4)
    m.inst(OP_TYPE_POINTER, ptr_pc, SC_PUSH_CONSTANT, pc_struct)
    m.inst(OP_TYPE_POINTER, ptr_pc_mat, SC_PUSH_CONSTANT, mat4)
    m.inst(OP_TYPE_POINTER, ptr_pc_vec4, SC_PUSH_CONSTANT, vec4)
    m.inst(OP_TYPE_POINTER, ptr_in3, SC_INPUT, vec3)
    m.inst(OP_TYPE_POINTER, ptr_in2, SC_INPUT, vec2)
    m.inst(OP_TYPE_POINTER, ptr_in4, SC_INPUT, vec4)
    m.inst(OP_TYPE_POINTER, ptr_out2, SC_OUTPUT, vec2)
    m.inst(OP_TYPE_POINTER, ptr_out4, SC_OUTPUT, vec4)
    m.inst(OP_CONSTANT, flt, const1, fbits(1.0))
    m.inst(OP_CONSTANT, i32, const_i0, 0)
    m.inst(OP_CONSTANT, i32, const_i1, 1)
    m.inst(OP_CONSTANT, u32, const_u1, 1)
    m.inst(OP_TYPE_ARRAY, arr_f1, flt, const_u1)
    m.inst(OP_TYPE_POINTER, ptr_out_arr, SC_OUTPUT, arr_f1)
    m.inst(OP_TYPE_POINTER, ptr_out_f, SC_OUTPUT, flt)

    m.inst(OP_VARIABLE, ptr_in3, in_pos, SC_INPUT)
    m.inst(OP_VARIABLE, ptr_in2, in_uv, SC_INPUT)
    m.inst(OP_VARIABLE, ptr_in2, in_params, SC_INPUT)
    m.inst(OP_VARIABLE, ptr_in4, in_color, SC_INPUT)
    m.inst(OP_VARIABLE, ptr_in2, in_uv2, SC_INPUT)
    m.inst(OP_VARIABLE, ptr_out2, v_uv, SC_OUTPUT)
    m.inst(OP_VARIABLE, ptr_out4, v_color, SC_OUTPUT)
    m.inst(OP_VARIABLE, ptr_out2, v_params, SC_OUTPUT)
    m.inst(OP_VARIABLE, ptr_out2, v_uv2, SC_OUTPUT)
    m.inst(OP_VARIABLE, ptr_out4, gl_pos, SC_OUTPUT)
    m.inst(OP_VARIABLE, ptr_pc, pc, SC_PUSH_CONSTANT)
    m.inst(OP_VARIABLE, ptr_out_arr, clip_dist, SC_OUTPUT)

    m.inst(OP_FUNCTION, void, fn_main, 0, fn_type)
    m.inst(OP_LABEL, label)
    m.inst(OP_LOAD, vec3, pos_val, in_pos)
    m.inst(OP_COMPOSITE_EXTRACT, flt, x_val, pos_val, 0)
    m.inst(OP_COMPOSITE_EXTRACT, flt, y_val, pos_val, 1)
    m.inst(OP_COMPOSITE_EXTRACT, flt, z_val, pos_val, 2)
    m.inst(OP_COMPOSITE_CONSTRUCT, vec4, pos4, x_val, y_val, z_val, const1)
    m.inst(OP_ACCESS_CHAIN, ptr_pc_mat, mat_ptr, pc, const_i0)
    m.inst(OP_LOAD, mat4, mat_val, mat_ptr)
    m.inst(OP_MATRIX_TIMES_VECTOR, vec4, clip_val, mat_val, pos4)
    m.inst(OP_STORE, gl_pos, clip_val)
    m.inst(OP_ACCESS_CHAIN, ptr_pc_vec4, clip_ptr, pc, const_i1)
    m.inst(OP_LOAD, vec4, clip_vec, clip_ptr)
    m.inst(OP_DOT, flt, dist_val, clip_vec, pos4)
    m.inst(OP_ACCESS_CHAIN, ptr_out_f, dist_ptr, clip_dist, const_i0)
    m.inst(OP_STORE, dist_ptr, dist_val)
    m.inst(OP_LOAD, vec2, uv_val, in_uv)
    m.inst(OP_STORE, v_uv, uv_val)
    m.inst(OP_LOAD, vec4, color_val, in_color)
    m.inst(OP_STORE, v_color, color_val)
    m.inst(OP_LOAD, vec2, params_val, in_params)
    m.inst(OP_STORE, v_params, params_val)
    m.inst(OP_LOAD, vec2, uv2_val, in_uv2)
    m.inst(OP_STORE, v_uv2, uv2_val)
    m.inst(OP_RETURN)
    m.inst(OP_FUNCTION_END)
    m.words[3] = ids.next
    return m.words


def fragment_module():
    ids = Ids(1)
    void = ids.take()
    flt = ids.take()
    boolean = ids.take()
    vec2 = ids.take()
    vec4 = ids.take()
    fn_type = ids.take()
    ptr_in2 = ids.take()
    ptr_in2b = ids.take()
    ptr_in4 = ids.take()
    ptr_out4 = ids.take()
    image = ids.take()
    sampled_image = ids.take()
    ptr_uniform = ids.take()
    in_uv = ids.take()
    in_params = ids.take()
    in_color = ids.take()
    out_color = ids.take()
    tex = ids.take()
    tex2 = ids.take()
    in_uv2 = ids.take()
    fn_main = ids.take()
    label = ids.take()
    kill_label = ids.take()
    end_label = ids.take()

    const0 = ids.take()
    const_half = ids.take()
    func_consts = {code: ids.take() for code in
                   (GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
                    GL_GREATER, GL_NOTEQUAL, GL_GEQUAL)}

    uv_val = ids.take()
    params_val = ids.take()
    color_val = ids.take()
    sampled_val = ids.take()
    tex_val = ids.take()
    uv2_val = ids.take()
    sampled2_val = ids.take()
    tex2_val = ids.take()
    mod_val = ids.take()
    final_val = ids.take()
    ref_val = ids.take()
    func_val = ids.take()
    alpha_val = ids.take()

    cmp_ids = []
    and_ids = []
    kill_val = ids.take()

    m = Module(ids.next)
    m.inst(OP_CAPABILITY, 1)                     # Shader
    m.inst(OP_MEMORY_MODEL, 0, SC_GLSL450)       # Logical
    m.inst(OP_ENTRY_POINT, EXEC_FRAGMENT, fn_main, *string_words("main"),
           in_uv, in_params, in_color, out_color, in_uv2)
    m.inst(OP_EXECUTION_MODE, fn_main, MODE_ORIGIN_UPPER_LEFT)
    m.inst(OP_DECORATE, in_uv, DEC_LOCATION, 0)
    m.inst(OP_DECORATE, in_params, DEC_LOCATION, 2)
    m.inst(OP_DECORATE, in_color, DEC_LOCATION, 1)
    m.inst(OP_DECORATE, out_color, DEC_LOCATION, 0)
    m.inst(OP_DECORATE, tex, DEC_BINDING, 0)
    m.inst(OP_DECORATE, tex, DEC_DESCRIPTOR_SET, 0)
    m.inst(OP_DECORATE, tex2, DEC_BINDING, 0)
    m.inst(OP_DECORATE, tex2, DEC_DESCRIPTOR_SET, 1)
    m.inst(OP_DECORATE, in_uv2, DEC_LOCATION, 3)

    m.inst(OP_TYPE_VOID, void)
    m.inst(OP_TYPE_BOOL, boolean)
    m.inst(OP_TYPE_FLOAT, flt, 32)
    m.inst(OP_TYPE_VECTOR, vec2, flt, 2)
    m.inst(OP_TYPE_VECTOR, vec4, flt, 4)
    m.inst(OP_TYPE_FUNCTION, fn_type, void)
    m.inst(OP_TYPE_POINTER, ptr_in2, SC_INPUT, vec2)
    m.inst(OP_TYPE_POINTER, ptr_in2b, SC_INPUT, vec2)
    m.inst(OP_TYPE_POINTER, ptr_in4, SC_INPUT, vec4)
    m.inst(OP_TYPE_POINTER, ptr_out4, SC_OUTPUT, vec4)
    # OpTypeImage: float, 2D, not depth, not arrayed, not multisampled,
    # sampled=1, Unknown format.
    m.inst(OP_TYPE_IMAGE, image, flt, 1, 0, 0, 0, 1, 0)
    m.inst(OP_TYPE_SAMPLED_IMAGE, sampled_image, image)
    m.inst(OP_TYPE_POINTER, ptr_uniform, SC_UNIFORM_CONSTANT, sampled_image)

    m.inst(OP_CONSTANT, flt, const0, fbits(0.0))
    m.inst(OP_CONSTANT, flt, const_half, fbits(0.5))
    for code, cid in func_consts.items():
        m.inst(OP_CONSTANT, flt, cid, fbits(code))

    m.inst(OP_VARIABLE, ptr_in2, in_uv, SC_INPUT)
    m.inst(OP_VARIABLE, ptr_in2b, in_params, SC_INPUT)
    m.inst(OP_VARIABLE, ptr_in4, in_color, SC_INPUT)
    m.inst(OP_VARIABLE, ptr_out4, out_color, SC_OUTPUT)
    m.inst(OP_VARIABLE, ptr_uniform, tex, SC_UNIFORM_CONSTANT)
    m.inst(OP_VARIABLE, ptr_uniform, tex2, SC_UNIFORM_CONSTANT)
    m.inst(OP_VARIABLE, ptr_in2, in_uv2, SC_INPUT)

    m.inst(OP_FUNCTION, void, fn_main, 0, fn_type)
    m.inst(OP_LABEL, label)
    m.inst(OP_LOAD, vec2, uv_val, in_uv)
    m.inst(OP_LOAD, vec2, params_val, in_params)
    m.inst(OP_LOAD, vec4, color_val, in_color)
    m.inst(OP_LOAD, sampled_image, sampled_val, tex)
    m.inst(OP_IMAGE_SAMPLE_IMPLICIT_LOD, vec4, tex_val, sampled_val, uv_val)
    m.inst(OP_FMUL, vec4, mod_val, color_val, tex_val)
    # Texture unit 1 (lightmap).  When the unit is disabled the C side binds
    # the 1x1 white texture, so this multiply is a no-op.
    m.inst(OP_LOAD, vec2, uv2_val, in_uv2)
    m.inst(OP_LOAD, sampled_image, sampled2_val, tex2)
    m.inst(OP_IMAGE_SAMPLE_IMPLICIT_LOD, vec4, tex2_val, sampled2_val, uv2_val)
    m.inst(OP_FMUL, vec4, final_val, mod_val, tex2_val)

    m.inst(OP_COMPOSITE_EXTRACT, flt, ref_val, params_val, 0)
    m.inst(OP_COMPOSITE_EXTRACT, flt, func_val, params_val, 1)
    m.inst(OP_COMPOSITE_EXTRACT, flt, alpha_val, final_val, 3)

    # Build one boolean: true means this fragment fails GL_ALPHA_TEST.
    # The function code is passed as a float from vkskeleton.c.
    tests = [
        (GL_NEVER, None),
        (GL_LESS, OP_FORD_GREATER_THAN_EQUAL),
        (GL_EQUAL, OP_FORD_NOT_EQUAL),
        (GL_LEQUAL, OP_FORD_GREATER_THAN),
        (GL_GREATER, OP_FORD_LESS_THAN_EQUAL),
        (GL_NOTEQUAL, OP_FORD_EQUAL),
        (GL_GEQUAL, OP_FORD_LESS_THAN),
    ]
    for code, fail_op in tests:
        func_eq = ids.take()
        m.inst(OP_FORD_EQUAL, boolean, func_eq, func_val, func_consts[code])
        if fail_op is None:
            term = func_eq
        else:
            failed = ids.take()
            m.inst(fail_op, boolean, failed, alpha_val, ref_val)
            term = ids.take()
            m.inst(OP_LOGICAL_AND, boolean, term, func_eq, failed)
        cmp_ids.append(term)
    kill_val = cmp_ids[0]
    for term in cmp_ids[1:]:
        combined = ids.take()
        m.inst(OP_LOGICAL_OR, boolean, combined, kill_val, term)
        kill_val = combined

    m.inst(OP_SELECTION_MERGE, end_label, 0)
    m.inst(OP_BRANCH_CONDITIONAL, kill_val, kill_label, end_label)
    m.inst(OP_LABEL, kill_label)
    m.inst(OP_KILL)
    m.inst(OP_LABEL, end_label)
    m.inst(OP_STORE, out_color, final_val)
    m.inst(OP_RETURN)
    m.inst(OP_FUNCTION_END)
    m.words[3] = ids.next
    return m.words



OP_TYPE_INT_ = OP_TYPE_INT
OP_CONVERT_F_TO_S = 110
OP_IADD = 128
OP_FADD = 129
OP_FSUB = 131
OP_IMUL = 132
OP_FDIV = 136
OP_SHIFT_RIGHT_ARITHMETIC = 195
OP_BITWISE_AND = 199
OP_VECTOR_EXTRACT_DYNAMIC = 77
OP_SELECT = 169
DEC_ARRAY_STRIDE = 6
SC_UNIFORM = 2

SKIN_BONE_BASE = 4      # vec4 index of bone 0 in the per-draw block
SKIN_DRAW_VEC4 = 4 + 128 * 4
LIGHT_SLOTS = 1025      # must match vkskeleton.c
FULLBRIGHT_SLOT = LIGHT_SLOTS - 1


def skin_vertex_module():
    """GoldSrc studio model skinning + lighting on the GPU.

    Same outputs as vertex_module (the fragment shader is shared).  Per
    vertex: model-space position, uv, model-space normal, and
    (bone, normal bone, flags) with flags 0 smooth / 1 flatshade /
    2 fullbright.  Per draw (set 2, binding 0, dynamic UBO, vec4 d[]):
      d[0] = ambient, shadelight, lambert      d[1] = light colour rgb, alpha
      d[2] = alpha_ref, alpha_func             d[4+4i..] = bone i rows 0-2
      (3x4 bone->world) and row 3 = light vector in bone i's space.
    Binding 1: the engine's light value per (int)(illum*4) slot, packed
    4 per vec4.  Equivalent C: studio_draw_fast().
    """
    ids = Ids(1)
    T = {}
    for n in ("void flt i32 u32 boolean vec2 vec3 vec4 mat4 fn pc_struct ptr_pc "
              "ptr_pc_mat ptr_pc_vec4 ptr_in3 ptr_in2 ptr_in4 ptr_out2 ptr_out4 "
              "arr_f1 ptr_out_arr ptr_out_f arr_d arr_l blk_d blk_l ptr_blk_d "
              "ptr_blk_l ptr_u_vec4").split():
        T[n] = ids.take()
    C = {}
    def cf(v):
        key = ("f", v)
        if key not in C:
            C[key] = ids.take()
        return C[key]
    def ci(v):
        key = ("i", v)
        if key not in C:
            C[key] = ids.take()
        return C[key]
    def cu(v):
        key = ("u", v)
        if key not in C:
            C[key] = ids.take()
        return C[key]
    for v in (0.0, 1.0, 0.8, 255.0, 4.0, 2.0):
        cf(v)
    for v in (0, 1, 2, 3, 4, SKIN_BONE_BASE, SKIN_BONE_BASE + 3, FULLBRIGHT_SLOT):
        ci(v)
    for v in (1, SKIN_DRAW_VEC4, (LIGHT_SLOTS + 3) // 4):
        cu(v)
    V = {}
    for n in ("in_pos in_uv in_nrm in_ex v_uv v_color v_params v_uv2 gl_pos "
              "clip_dist pc ubo_d ubo_l fn_main label").split():
        V[n] = ids.take()

    body = []
    def op(opcode, rtype, *args):
        r = ids.take()
        body.append((opcode, rtype, r) + args)
        return r
    def st(ptr, val):
        body.append((OP_STORE, ptr, val))

    f, i32, v4 = T["flt"], T["i32"], T["vec4"]
    def ld_d(idx):           # u.d[idx]
        ptr = op(OP_ACCESS_CHAIN, T["ptr_u_vec4"], V["ubo_d"], ci(0), idx)
        return op(OP_LOAD, v4, ptr)
    def ex(vec, k, t=f):
        return op(OP_COMPOSITE_EXTRACT, t, vec, k)
    def sel(cond, a, b, t=f):
        return op(OP_SELECT, t, cond, a, b)

    pos = op(OP_LOAD, T["vec3"], V["in_pos"])
    p4 = op(OP_COMPOSITE_CONSTRUCT, v4, ex(pos, 0), ex(pos, 1), ex(pos, 2), cf(1.0))
    exv = op(OP_LOAD, v4, V["in_ex"])
    bone = op(OP_CONVERT_F_TO_S, i32, ex(exv, 0))
    b4 = op(OP_IMUL, i32, bone, ci(4))
    r0i = op(OP_IADD, i32, b4, ci(SKIN_BONE_BASE))
    r1i = op(OP_IADD, i32, r0i, ci(1))
    r2i = op(OP_IADD, i32, r0i, ci(2))
    wx = op(OP_DOT, f, ld_d(r0i), p4)
    wy = op(OP_DOT, f, ld_d(r1i), p4)
    wz = op(OP_DOT, f, ld_d(r2i), p4)
    w4 = op(OP_COMPOSITE_CONSTRUCT, v4, wx, wy, wz, cf(1.0))
    mptr = op(OP_ACCESS_CHAIN, T["ptr_pc_mat"], V["pc"], ci(0))
    mvp = op(OP_LOAD, T["mat4"], mptr)
    st(V["gl_pos"], op(OP_MATRIX_TIMES_VECTOR, v4, mvp, w4))
    cptr = op(OP_ACCESS_CHAIN, T["ptr_pc_vec4"], V["pc"], ci(1))
    cplane = op(OP_LOAD, v4, cptr)
    dptr = op(OP_ACCESS_CHAIN, T["ptr_out_f"], V["clip_dist"], ci(0))
    st(dptr, op(OP_DOT, f, cplane, w4))

    # lighting (GoldSrc StudioLighting)
    nbone = op(OP_CONVERT_F_TO_S, i32, ex(exv, 1))
    nb4 = op(OP_IMUL, i32, nbone, ci(4))
    lvi = op(OP_IADD, i32, nb4, ci(SKIN_BONE_BASE + 3))
    lvec = ld_d(lvi)
    nrm = op(OP_LOAD, T["vec3"], V["in_nrm"])
    n4 = op(OP_COMPOSITE_CONSTRUCT, v4, ex(nrm, 0), ex(nrm, 1), ex(nrm, 2), cf(0.0))
    h0 = ld_d(ci(0))
    amb, shade, lam = ex(h0, 0), ex(h0, 1), ex(h0, 2)
    lc0 = op(OP_DOT, f, n4, lvec)
    lc1 = sel(op(OP_FORD_GREATER_THAN, T["boolean"], lc0, cf(1.0)), cf(1.0), lc0)
    lam1 = op(OP_FSUB, f, lam, cf(1.0))
    lc2 = op(OP_FDIV, f, op(OP_FADD, f, lc1, lam1), lam)
    lcp = sel(op(OP_FORD_GREATER_THAN, T["boolean"], lc2, cf(0.0)), lc2, cf(0.0))
    base = op(OP_FADD, f, amb, shade)
    smooth = op(OP_FSUB, f, base, op(OP_FMUL, f, shade, lcp))
    flat = op(OP_FADD, f, amb, op(OP_FMUL, f, shade, cf(0.8)))
    flags = ex(exv, 2)
    il0 = sel(op(OP_FORD_EQUAL, T["boolean"], flags, cf(1.0)), flat, smooth)
    il1 = sel(op(OP_FORD_LESS_THAN, T["boolean"], il0, cf(0.0)), cf(0.0), il0)
    il2 = sel(op(OP_FORD_GREATER_THAN, T["boolean"], il1, cf(255.0)), cf(255.0), il1)
    slot0 = op(OP_CONVERT_F_TO_S, i32, op(OP_FMUL, f, il2, cf(4.0)))
    slot = sel(op(OP_FORD_EQUAL, T["boolean"], flags, cf(2.0)), ci(FULLBRIGHT_SLOT),
               slot0, i32)
    li = op(OP_SHIFT_RIGHT_ARITHMETIC, i32, slot, ci(2))
    lk = op(OP_BITWISE_AND, i32, slot, ci(3))
    lptr = op(OP_ACCESS_CHAIN, T["ptr_u_vec4"], V["ubo_l"], ci(0), li)
    lv = op(OP_VECTOR_EXTRACT_DYNAMIC, f, op(OP_LOAD, v4, lptr), lk)
    h1 = ld_d(ci(1))
    col = op(OP_COMPOSITE_CONSTRUCT, v4, op(OP_FMUL, f, ex(h1, 0), lv),
             op(OP_FMUL, f, ex(h1, 1), lv), op(OP_FMUL, f, ex(h1, 2), lv), ex(h1, 3))
    st(V["v_color"], col)
    h2 = ld_d(ci(2))
    st(V["v_params"], op(OP_COMPOSITE_CONSTRUCT, T["vec2"], ex(h2, 0), ex(h2, 1)))
    st(V["v_uv"], op(OP_LOAD, T["vec2"], V["in_uv"]))
    st(V["v_uv2"], op(OP_COMPOSITE_CONSTRUCT, T["vec2"], cf(0.0), cf(0.0)))

    m = Module(ids.next)
    m.inst(OP_CAPABILITY, 1)
    m.inst(OP_CAPABILITY, 32)                    # ClipDistance
    m.inst(OP_MEMORY_MODEL, 0, SC_GLSL450)
    m.inst(OP_ENTRY_POINT, EXEC_VERTEX, V["fn_main"], *string_words("main"),
           V["in_pos"], V["in_uv"], V["in_nrm"], V["in_ex"], V["v_uv"],
           V["v_color"], V["v_params"], V["v_uv2"], V["gl_pos"], V["clip_dist"])
    for n, loc in (("in_pos", 0), ("in_uv", 1), ("in_nrm", 2), ("in_ex", 3),
                   ("v_uv", 0), ("v_color", 1), ("v_params", 2), ("v_uv2", 3)):
        m.inst(OP_DECORATE, V[n], DEC_LOCATION, loc)
    m.inst(OP_DECORATE, V["gl_pos"], DEC_BUILTIN, BUILTIN_POSITION)
    m.inst(OP_DECORATE, V["clip_dist"], DEC_BUILTIN, BUILTIN_CLIP_DISTANCE)
    m.inst(OP_DECORATE, T["pc_struct"], DEC_BLOCK)
    m.inst(OP_MEMBER_DECORATE, T["pc_struct"], 0, DEC_COL_MAJOR)
    m.inst(OP_MEMBER_DECORATE, T["pc_struct"], 0, DEC_OFFSET, 0)
    m.inst(OP_MEMBER_DECORATE, T["pc_struct"], 0, DEC_MATRIX_STRIDE, 16)
    m.inst(OP_MEMBER_DECORATE, T["pc_struct"], 1, DEC_OFFSET, 64)
    for arr, blk, var, binding in (("arr_d", "blk_d", "ubo_d", 0),
                                   ("arr_l", "blk_l", "ubo_l", 1)):
        m.inst(OP_DECORATE, T[arr], DEC_ARRAY_STRIDE, 16)
        m.inst(OP_DECORATE, T[blk], DEC_BLOCK)
        m.inst(OP_MEMBER_DECORATE, T[blk], 0, DEC_OFFSET, 0)
        m.inst(OP_DECORATE, V[var], DEC_DESCRIPTOR_SET, 2)
        m.inst(OP_DECORATE, V[var], DEC_BINDING, binding)

    m.inst(OP_TYPE_VOID, T["void"])
    m.inst(OP_TYPE_BOOL, T["boolean"])
    m.inst(OP_TYPE_FLOAT, T["flt"], 32)
    m.inst(OP_TYPE_INT, T["i32"], 32, 1)
    m.inst(OP_TYPE_INT, T["u32"], 32, 0)
    m.inst(OP_TYPE_VECTOR, T["vec2"], T["flt"], 2)
    m.inst(OP_TYPE_VECTOR, T["vec3"], T["flt"], 3)
    m.inst(OP_TYPE_VECTOR, T["vec4"], T["flt"], 4)
    m.inst(OP_TYPE_MATRIX, T["mat4"], T["vec4"], 4)
    m.inst(OP_TYPE_FUNCTION, T["fn"], T["void"])
    for (kind, v), cid in C.items():
        if kind == "f":
            m.inst(OP_CONSTANT, T["flt"], cid, fbits(v))
        elif kind == "i":
            m.inst(OP_CONSTANT, T["i32"], cid, v & 0xFFFFFFFF)
        else:
            m.inst(OP_CONSTANT, T["u32"], cid, v)
    m.inst(OP_TYPE_STRUCT, T["pc_struct"], T["mat4"], T["vec4"])
    m.inst(OP_TYPE_POINTER, T["ptr_pc"], SC_PUSH_CONSTANT, T["pc_struct"])
    m.inst(OP_TYPE_POINTER, T["ptr_pc_mat"], SC_PUSH_CONSTANT, T["mat4"])
    m.inst(OP_TYPE_POINTER, T["ptr_pc_vec4"], SC_PUSH_CONSTANT, T["vec4"])
    m.inst(OP_TYPE_POINTER, T["ptr_in3"], SC_INPUT, T["vec3"])
    m.inst(OP_TYPE_POINTER, T["ptr_in2"], SC_INPUT, T["vec2"])
    m.inst(OP_TYPE_POINTER, T["ptr_in4"], SC_INPUT, T["vec4"])
    m.inst(OP_TYPE_POINTER, T["ptr_out2"], SC_OUTPUT, T["vec2"])
    m.inst(OP_TYPE_POINTER, T["ptr_out4"], SC_OUTPUT, T["vec4"])
    m.inst(OP_TYPE_ARRAY, T["arr_f1"], T["flt"], cu(1))
    m.inst(OP_TYPE_POINTER, T["ptr_out_arr"], SC_OUTPUT, T["arr_f1"])
    m.inst(OP_TYPE_POINTER, T["ptr_out_f"], SC_OUTPUT, T["flt"])
    m.inst(OP_TYPE_ARRAY, T["arr_d"], T["vec4"], cu(SKIN_DRAW_VEC4))
    m.inst(OP_TYPE_ARRAY, T["arr_l"], T["vec4"], cu((LIGHT_SLOTS + 3) // 4))
    m.inst(OP_TYPE_STRUCT, T["blk_d"], T["arr_d"])
    m.inst(OP_TYPE_STRUCT, T["blk_l"], T["arr_l"])
    m.inst(OP_TYPE_POINTER, T["ptr_blk_d"], SC_UNIFORM, T["blk_d"])
    m.inst(OP_TYPE_POINTER, T["ptr_blk_l"], SC_UNIFORM, T["blk_l"])
    m.inst(OP_TYPE_POINTER, T["ptr_u_vec4"], SC_UNIFORM, T["vec4"])

    for n, ptr, sc in (("in_pos", "ptr_in3", SC_INPUT), ("in_uv", "ptr_in2", SC_INPUT),
                       ("in_nrm", "ptr_in3", SC_INPUT), ("in_ex", "ptr_in4", SC_INPUT),
                       ("v_uv", "ptr_out2", SC_OUTPUT), ("v_color", "ptr_out4", SC_OUTPUT),
                       ("v_params", "ptr_out2", SC_OUTPUT), ("v_uv2", "ptr_out2", SC_OUTPUT),
                       ("gl_pos", "ptr_out4", SC_OUTPUT),
                       ("clip_dist", "ptr_out_arr", SC_OUTPUT),
                       ("pc", "ptr_pc", SC_PUSH_CONSTANT),
                       ("ubo_d", "ptr_blk_d", SC_UNIFORM),
                       ("ubo_l", "ptr_blk_l", SC_UNIFORM)):
        m.inst(OP_VARIABLE, T[ptr], V[n], sc)

    m.inst(OP_FUNCTION, T["void"], V["fn_main"], 0, T["fn"])
    m.inst(OP_LABEL, V["label"])
    for ins in body:
        m.inst(*ins)
    m.inst(OP_RETURN)
    m.inst(OP_FUNCTION_END)
    m.words[3] = ids.next
    return m.words


def emit_array(name, words):
    lines = [f"static const uint32_t {name}[] = {{"]
    for i in range(0, len(words), 8):
        chunk = words[i:i + 8]
        lines.append("    " + ", ".join(f"0x{w:08X}u" for w in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    vert = vertex_module()
    frag = fragment_module()
    skin = skin_vertex_module()
    # usage: gen_shaders.py [output header]   (default: ../src/shaders_spirv.h)
    out = (Path(sys.argv[1]) if len(sys.argv) > 1 else
           Path(__file__).resolve().parent.parent / "src" / "shaders_spirv.h")
    text = f"""/* Generated by gen_shaders.py -- do not edit. */
#pragma once
#include <stdint.h>

{emit_array('vk_vertex_spirv', vert)}

{emit_array('vk_fragment_spirv', frag)}

{emit_array('vk_skin_vertex_spirv', skin)}

#define VK_VERTEX_SPIRV_BYTES  (sizeof(vk_vertex_spirv))
#define VK_FRAGMENT_SPIRV_BYTES (sizeof(vk_fragment_spirv))
#define VK_SKIN_VERTEX_SPIRV_BYTES (sizeof(vk_skin_vertex_spirv))
"""
    out.write_text(text)
    print(f"wrote: {out} ({len(vert)} vertex words, {len(frag)} fragment words)")


if __name__ == "__main__":
    main()
