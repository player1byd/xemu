/*
 * Geforce NV2A PGRAPH GLSL Shader Generator
 *
 * Copyright (c) 2014 Jannik Vogel
 * Copyright (c) 2012 espes
 * Copyright (c) 2025 Matt Borgerson
 *
 * Based on:
 * Cxbx, VertexShader.cpp
 * Copyright (c) 2004 Aaron Robinson <caustik@caustik.com>
 *                    Kingofc <kingofc@freenet.de>
 * Dxbx, uPushBuffer.pas
 * Copyright (c) 2007 Shadow_tj, PatrickvL
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include "common.h"
#include "vsh.h"
#include "vsh-prog.h"

typedef struct VshFieldMapping {
    VshFieldName field_name;
    uint8_t subtoken;
    uint8_t start_bit;
    uint8_t bit_length;
} VshFieldMapping;

static const VshFieldMapping field_mapping[] = {
    // Field Name         DWORD BitPos BitSize
    {  FLD_ILU,              1,   25,     3 },
    {  FLD_MAC,              1,   21,     4 },
    {  FLD_CONST,            1,   13,     8 },
    {  FLD_V,                1,    9,     4 },
    // INPUT A
    {  FLD_A_NEG,            1,    8,     1 },
    {  FLD_A_SWZ_X,          1,    6,     2 },
    {  FLD_A_SWZ_Y,          1,    4,     2 },
    {  FLD_A_SWZ_Z,          1,    2,     2 },
    {  FLD_A_SWZ_W,          1,    0,     2 },
    {  FLD_A_R,              2,   28,     4 },
    {  FLD_A_MUX,            2,   26,     2 },
    // INPUT B
    {  FLD_B_NEG,            2,   25,     1 },
    {  FLD_B_SWZ_X,          2,   23,     2 },
    {  FLD_B_SWZ_Y,          2,   21,     2 },
    {  FLD_B_SWZ_Z,          2,   19,     2 },
    {  FLD_B_SWZ_W,          2,   17,     2 },
    {  FLD_B_R,              2,   13,     4 },
    {  FLD_B_MUX,            2,   11,     2 },
    // INPUT C
    {  FLD_C_NEG,            2,   10,     1 },
    {  FLD_C_SWZ_X,          2,    8,     2 },
    {  FLD_C_SWZ_Y,          2,    6,     2 },
    {  FLD_C_SWZ_Z,          2,    4,     2 },
    {  FLD_C_SWZ_W,          2,    2,     2 },
    {  FLD_C_R_HIGH,         2,    0,     2 },
    {  FLD_C_R_LOW,          3,   30,     2 },
    {  FLD_C_MUX,            3,   28,     2 },
    // Output
    {  FLD_OUT_MAC_MASK,     3,   24,     4 },
    {  FLD_OUT_R,            3,   20,     4 },
    {  FLD_OUT_ILU_MASK,     3,   16,     4 },
    {  FLD_OUT_O_MASK,       3,   12,     4 },
    {  FLD_OUT_ORB,          3,   11,     1 },
    {  FLD_OUT_ADDRESS,      3,    3,     8 },
    {  FLD_OUT_MUX,          3,    2,     1 },
    // Other
    {  FLD_A0X,              3,    1,     1 },
    {  FLD_FINAL,            3,    0,     1 }
};

typedef struct VshOpcodeParams {
    bool A;
    bool B;
    bool C;
} VshOpcodeParams;

#if 0
static const VshOpcodeParams ilu_opcode_params[] = {
    /* ILU OP       ParamA ParamB ParamC */
    /* ILU_NOP */ { false, false, false }, // Dxbx note : Unused
    /* ILU_MOV */ { false, false, true  },
    /* ILU_RCP */ { false, false, true  },
    /* ILU_RCC */ { false, false, true  },
    /* ILU_RSQ */ { false, false, true  },
    /* ILU_EXP */ { false, false, true  },
    /* ILU_LOG */ { false, false, true  },
    /* ILU_LIT */ { false, false, true  },
};
#endif

static const VshOpcodeParams mac_opcode_params[] = {
    /* MAC OP      ParamA  ParamB ParamC */
    /* MAC_NOP */ { false, false, false }, // Dxbx note : Unused
    /* MAC_MOV */ { true,  false, false },
    /* MAC_MUL */ { true,  true,  false },
    /* MAC_ADD */ { true,  false, true  },
    /* MAC_MAD */ { true,  true,  true  },
    /* MAC_DP3 */ { true,  true,  false },
    /* MAC_DPH */ { true,  true,  false },
    /* MAC_DP4 */ { true,  true,  false },
    /* MAC_DST */ { true,  true,  false },
    /* MAC_MIN */ { true,  true,  false },
    /* MAC_MAX */ { true,  true,  false },
    /* MAC_SLT */ { true,  true,  false },
    /* MAC_SGE */ { true,  true,  false },
    /* MAC_ARL */ { true,  false, false },
};


static const char* mask_str[] = {
            // xyzw xyzw
    ",",     // 0000 ____
    ",w",   // 0001 ___w
    ",z",   // 0010 __z_
    ",zw",  // 0011 __zw
    ",y",   // 0100 _y__
    ",yw",  // 0101 _y_w
    ",yz",  // 0110 _yz_
    ",yzw", // 0111 _yzw
    ",x",   // 1000 x___
    ",xw",  // 1001 x__w
    ",xz",  // 1010 x_z_
    ",xzw", // 1011 x_zw
    ",xy",  // 1100 xy__
    ",xyw", // 1101 xy_w
    ",xyz", // 1110 xyz_
    ",xyzw" // 1111 xyzw
};

/* Writes to the oFog register apply the most significant masked component to
 * `x`. The remaining values are assigned arbitrarily to fit the 4-component
 * function behavior. */
static const char* fog_mask_str[] = {
    ",",
    ",x",
    ",x",
    ",xy",
    ",x",
    ",xy",
    ",xy",
    ",xyz",
    ",x",
    ",xy",
    ",xy",
    ",xyz",
    ",xy",
    ",xyz",
    ",xyz",
    ",xyzw"
};

/* Note: OpenGL seems to be case-sensitive, and requires upper-case opcodes! */
static const char* mac_opcode[] = {
    "NOP",
    "MOV",
    "MUL",
    "ADD",
    "MAD",
    "DP3",
    "DPH",
    "DP4",
    "DST",
    "MIN",
    "MAX",
    "SLT",
    "SGE",
    "ARL A0.x", // Dxbx note : Alias for "mov a0.x"
};

static const char* ilu_opcode[] = {
    "NOP",
    "MOV",
    "RCP",
    "RCC",
    "RSQ",
    "EXP",
    "LOG",
    "LIT",
};

static bool ilu_force_scalar[] = {
    false,
    false,
    true,
    true,
    true,
    true,
    true,
    false,
};

#define OUTPUT_REG_FOG 5

static const char* out_reg_name[] = {
    "oPos",
    "???",
    "???",
    "oD0",
    "oD1",
    "oFog",
    "oPts",
    "oB0",
    "oB1",
    "oT0",
    "oT1",
    "oT2",
    "oT3",
    "???",
    "???",
    "A0.x",
};

// Retrieves a number of bits in the instruction token
static int vsh_get_from_token(const uint32_t *shader_token,
                              uint8_t subtoken,
                              uint8_t start_bit,
                              uint8_t bit_length)
{
    return (shader_token[subtoken] >> start_bit) & ~(0xFFFFFFFF << bit_length);
}

uint8_t vsh_get_field(const uint32_t *shader_token, VshFieldName field_name)
{

    return (uint8_t)(vsh_get_from_token(shader_token,
                                        field_mapping[field_name].subtoken,
                                        field_mapping[field_name].start_bit,
                                        field_mapping[field_name].bit_length));
}

// Converts the C register address to disassembly format
static int16_t convert_c_register(const int16_t c_reg)
{
    int16_t r = ((((c_reg >> 5) & 7) - 3) * 32) + (c_reg & 31);
    r += VSH_D3DSCM_CORRECTION; /* to map -96..95 to 0..191 */
    return r; //FIXME: = c_reg?!
}

static MString *decode_swizzle(const uint32_t *shader_token,
                               VshFieldName swizzle_field)
{
    const char* swizzle_str = "xyzw";
    VshSwizzle x, y, z, w;

    /* some microcode instructions force a scalar value */
    if (swizzle_field == FLD_C_SWZ_X
        && ilu_force_scalar[vsh_get_field(shader_token, FLD_ILU)]) {
        x = y = z = w = vsh_get_field(shader_token, swizzle_field);
    } else {
        x = vsh_get_field(shader_token, swizzle_field++);
        y = vsh_get_field(shader_token, swizzle_field++);
        z = vsh_get_field(shader_token, swizzle_field++);
        w = vsh_get_field(shader_token, swizzle_field);
    }

    if (x == SWIZZLE_X && y == SWIZZLE_Y
        && z == SWIZZLE_Z && w == SWIZZLE_W) {
        /* Don't print the swizzle if it's .xyzw */
        return mstring_from_str(""); // Will turn ".xyzw" into "."
    /* Don't print duplicates */
    } else if (x == y && y == z && z == w) {
        return mstring_from_str((char[]){'.', swizzle_str[x], '\0'});
    } else if (y == z && z == w) {
        return mstring_from_str((char[]){'.',
            swizzle_str[x], swizzle_str[y], '\0'});
    } else if (z == w) {
        return mstring_from_str((char[]){'.',
            swizzle_str[x], swizzle_str[y], swizzle_str[z], '\0'});
    } else {
        return mstring_from_str((char[]){'.',
                                       swizzle_str[x], swizzle_str[y],
                                       swizzle_str[z], swizzle_str[w],
                                       '\0'}); // Normal swizzle mask
    }
}

static MString *decode_opcode_input(const uint32_t *shader_token,
                                    VshParameterType param,
                                    VshFieldName neg_field, int reg_num)
{
    /* This function decodes a vertex shader opcode parameter into a string.
     * Input A, B or C is controlled via the Param and NEG fieldnames,
     * the R-register address for each input is already given by caller. */

    MString *ret_str = mstring_new();


    if (vsh_get_field(shader_token, neg_field) > 0) {
        mstring_append_fmt(ret_str, "-");
    }

    /* PARAM_R uses the supplied reg_num, but the other two need to be
     * determined */
    char tmp[40];
    switch (param) {
    case PARAM_R:
        snprintf(tmp, sizeof(tmp), "R%d", reg_num);
        break;
    case PARAM_V:
        reg_num = vsh_get_field(shader_token, FLD_V);
        snprintf(tmp, sizeof(tmp), "v%d", reg_num);
        break;
    case PARAM_C:
        reg_num = convert_c_register(vsh_get_field(shader_token, FLD_CONST));
        if (vsh_get_field(shader_token, FLD_A0X) > 0) {
            //FIXME: does this really require the "correction" doe in convert_c_register?!
            snprintf(tmp, sizeof(tmp), "c[A0+%d]", reg_num);
        } else {
            snprintf(tmp, sizeof(tmp), "c[%d]", reg_num);
        }
        break;
    default:
        fprintf(stderr, "Unknown vs param: 0x%x\n", param);
        assert(!"Unknown vs param");
        break;
    }
    mstring_append(ret_str, tmp);

    {
        /* swizzle bits are next to the neg bit */
        MString *swizzle_str = decode_swizzle(shader_token, neg_field+1);
        mstring_append(ret_str, mstring_get_str(swizzle_str));
        mstring_unref(swizzle_str);
    }

    return ret_str;
}

static MString *decode_opcode(const uint32_t *shader_token,
                              VshOutputMux out_mux, uint32_t mask,
                              const char *opcode, const char *inputs,
                              MString **suffix)
{
    MString *ret = mstring_new();
    int reg_num = vsh_get_field(shader_token, FLD_OUT_R);
    bool use_temp_var = false;

    /* Test for paired opcodes (in other words : Are both <> NOP?) */
    if (out_mux == OMUX_MAC
          && vsh_get_field(shader_token, FLD_ILU) != ILU_NOP) {
        use_temp_var = true;
        if (reg_num == 1) {
            /* Ignore paired MAC opcodes that write to R1 */
            mask = 0;
        }
    } else if (out_mux == OMUX_ILU
               && vsh_get_field(shader_token, FLD_MAC) != MAC_NOP) {
        /* Paired ILU opcodes can only write to R1 */
        reg_num = 1;
    }

    /* See if we must add a muxed opcode too: */
    if (vsh_get_field(shader_token, FLD_OUT_MUX) == out_mux
        /* Only if it's not masked away: */
        && vsh_get_field(shader_token, FLD_OUT_O_MASK) != 0) {

        mstring_append(ret, "  ");
        mstring_append(ret, opcode);
        mstring_append(ret, "(");

        bool write_fog_register = false;
        if (vsh_get_field(shader_token, FLD_OUT_ORB) == OUTPUT_C) {
            assert(!"TODO: Emulate writeable const registers");
            mstring_append_fmt(ret, "c%d",
                               convert_c_register(vsh_get_field(
                                   shader_token, FLD_OUT_ADDRESS)));
        } else {
            int out_reg = vsh_get_field(shader_token, FLD_OUT_ADDRESS) & 0xF;
            mstring_append(ret,out_reg_name[out_reg]);
            write_fog_register = out_reg == OUTPUT_REG_FOG;
        }

        int write_mask = vsh_get_field(shader_token, FLD_OUT_O_MASK);
        const char *write_mask_str = write_fog_register ? fog_mask_str[write_mask] : mask_str[write_mask];
        mstring_append(ret, write_mask_str);
        mstring_append(ret, inputs);
        mstring_append(ret, ");\n");
    }

    if (use_temp_var) {
        assert(suffix && "Temp var flagged on non-MAC instruction");
        *suffix = mstring_new();
        if (strcmp(opcode, mac_opcode[MAC_ARL]) == 0) {
            mstring_append_fmt(ret, "  ARL(_temp_addr%s);\n", inputs);
            mstring_append(*suffix, "  A0 = _temp_addr;\n");
        } else if (mask > 0) {
            mstring_append_fmt(ret, "  %s(_temp_vec%s%s);\n",
                               opcode, mask_str[mask], inputs);

            // Skip the leading comma
            const char *mask_components = &mask_str[mask][1];
            if (mask_components[0]) {
                mstring_append_fmt(*suffix,
                                   "  R%d.%s = _temp_vec.%s;\n",
                                   reg_num,
                                   mask_components,
                                   mask_components);
            } else {
                mstring_append_fmt(*suffix, "  R%d = _temp_vec;\n", reg_num);
            }
        }
    } else {
        if (strcmp(opcode, mac_opcode[MAC_ARL]) == 0) {
            mstring_append_fmt(ret, "  ARL(A0%s);\n", inputs);
        } else if (mask > 0) {
            mstring_append_fmt(ret, "  %s(R%d%s%s);\n",
                               opcode, reg_num, mask_str[mask], inputs);
        }
    }

    return ret;
}

static MString *decode_token(const uint32_t *shader_token)
{
    MString *ret;

    /* See what MAC opcode is written to (if not masked away): */
    VshMAC mac = vsh_get_field(shader_token, FLD_MAC);
    /* See if a ILU opcode is present too: */
    VshILU ilu = vsh_get_field(shader_token, FLD_ILU);
    if (mac == MAC_NOP && ilu == ILU_NOP) {
        return mstring_new();
    }

    /* Since it's potentially used twice, decode input C once: */
    MString *input_c =
        decode_opcode_input(shader_token,
                            vsh_get_field(shader_token, FLD_C_MUX),
                            FLD_C_NEG,
                            (vsh_get_field(shader_token, FLD_C_R_HIGH) << 2)
                                | vsh_get_field(shader_token, FLD_C_R_LOW));

    MString *mac_suffix = NULL;
    if (mac != MAC_NOP) {
        MString *inputs_mac = mstring_new();
        if (mac_opcode_params[mac].A) {
            MString *input_a =
                decode_opcode_input(shader_token,
                                    vsh_get_field(shader_token, FLD_A_MUX),
                                    FLD_A_NEG,
                                    vsh_get_field(shader_token, FLD_A_R));
            mstring_append(inputs_mac, ", ");
            mstring_append(inputs_mac, mstring_get_str(input_a));
            mstring_unref(input_a);
        }
        if (mac_opcode_params[mac].B) {
            MString *input_b =
                decode_opcode_input(shader_token,
                                    vsh_get_field(shader_token, FLD_B_MUX),
                                    FLD_B_NEG,
                                    vsh_get_field(shader_token, FLD_B_R));
            mstring_append(inputs_mac, ", ");
            mstring_append(inputs_mac, mstring_get_str(input_b));
            mstring_unref(input_b);
        }
        if (mac_opcode_params[mac].C) {
            mstring_append(inputs_mac, ", ");
            mstring_append(inputs_mac, mstring_get_str(input_c));
        }

        /* Then prepend these inputs with the actual opcode, mask, and input : */
        ret = decode_opcode(shader_token,
                            OMUX_MAC,
                            vsh_get_field(shader_token, FLD_OUT_MAC_MASK),
                            mac_opcode[mac],
                            mstring_get_str(inputs_mac),
                            &mac_suffix);
        mstring_unref(inputs_mac);
    } else {
        ret = mstring_new();
    }

    if (ilu != ILU_NOP) {
        MString *inputs_c = mstring_from_str(", ");
        mstring_append(inputs_c, mstring_get_str(input_c));

        /* Append the ILU opcode, mask and (the already determined) input C: */
        MString *ilu_op =
            decode_opcode(shader_token,
                          OMUX_ILU,
                          vsh_get_field(shader_token, FLD_OUT_ILU_MASK),
                          ilu_opcode[ilu],
                          mstring_get_str(inputs_c),
                          NULL);

        mstring_append(ret, mstring_get_str(ilu_op));

        mstring_unref(inputs_c);
        mstring_unref(ilu_op);
    }

    mstring_unref(input_c);

    if (mac_suffix) {
        mstring_append(ret, mstring_get_str(mac_suffix));
        mstring_unref(mac_suffix);
    }

    return ret;
}

static const char* vsh_header =
    "\n"
    "int A0 = 0;\n"
    "\n"
    "vec4 R0 = vec4(0.0,0.0,0.0,0.0);\n"
    "vec4 R1 = vec4(0.0,0.0,0.0,0.0);\n"
    "vec4 R2 = vec4(0.0,0.0,0.0,0.0);\n"
    "vec4 R3 = vec4(0.0,0.0,0.0,0.0);\n"
    "vec4 R4 = vec4(0.0,0.0,0.0,0.0);\n"
    "vec4 R5 = vec4(0.0,0.0,0.0,0.0);\n"
    "vec4 R6 = vec4(0.0,0.0,0.0,0.0);\n"
    "vec4 R7 = vec4(0.0,0.0,0.0,0.0);\n"
    "vec4 R8 = vec4(0.0,0.0,0.0,0.0);\n"
    "vec4 R9 = vec4(0.0,0.0,0.0,0.0);\n"
    "vec4 R10 = vec4(0.0,0.0,0.0,0.0);\n"
    "vec4 R11 = vec4(0.0,0.0,0.0,0.0);\n"
    "#define R12 oPos\n" /* R12 is a mirror of oPos */
    "\n"

    /* Used to emulate concurrency of paired MAC+ILU instructions */
    "vec4 _temp_vec;\n"
    "int _temp_addr;\n"

    /* See:
     * http://msdn.microsoft.com/en-us/library/windows/desktop/bb174703%28v=vs.85%29.aspx
     * https://www.opengl.org/registry/specs/NV/vertex_program1_1.txt
     */
    "\n"
//QQQ #ifdef NICE_CODE
    "/* Converts the input to vec4, pads with last component */\n"
    "vec4 _in(float v) { return vec4(v); }\n"
    "vec4 _in(vec2 v) { return v.xyyy; }\n"
    "vec4 _in(vec3 v) { return v.xyzz; }\n"
    "vec4 _in(vec4 v) { return v.xyzw; }\n"
//#else
//    "/* Make sure input is always a vec4 */\n"
//   "#define _in(v) vec4(v)\n"
//#endif
    "\n"
    "#define INFINITY (1.0 / 0.0)\n"
    "\n"
    /*
     * The NV20 arithmetic algorithms and reciprocal lookup table below are
     * adapted from Envytools nvhw/fp.c and nvhw/xf.c at
     * f102b82381f3f11cee113d16374c87091db039d9:
     *
     * Copyright (C) 2016, 2018 Marcelina Kościelnicka <mwk@0x04.net>
     *
     * Permission is hereby granted, free of charge, to any person obtaining a
     * copy of this software and associated documentation files (the
     * "Software"), to deal in the Software without restriction, including
     * without limitation the rights to use, copy, modify, merge, publish,
     * distribute, sublicense, and/or sell copies of the Software, and to
     * permit persons to whom the Software is furnished to do so, subject to
     * the following conditions:
     *
     * The above copyright notice and this permission notice shall be included
     * in all copies or substantial portions of the Software.
     *
     * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
     * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
     * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
     * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
     * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
     * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
     * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
     */
    "#ifdef NV2A_NV20_ARITHMETIC\n"
    "uint nv20_exp_bits(uint v) { return (v >> 23) & 0xffu; }\n"
    "uint nv20_fract_bits(uint v) { return v & 0x7fffffu; }\n"
    "bool nv20_is_nan(uint v) {\n"
    "  return nv20_exp_bits(v) == 0xffu && nv20_fract_bits(v) != 0u;\n"
    "}\n"
    "bool nv20_is_inf(uint v) {\n"
    "  return nv20_exp_bits(v) == 0xffu && nv20_fract_bits(v) == 0u;\n"
    "}\n"
    "uint nv20_make_finite(bool sign_bit, int exponent, uint fraction) {\n"
    "  if (fraction == 0u) { exponent = 1; }\n"
    "  if (fraction == 0x1000000u) { fraction >>= 1; exponent++; }\n"
    "  if (exponent <= 0 || fraction < 0x800000u) {\n"
    "    exponent = 0; fraction = 0u;\n"
    "  } else if (exponent >= 0xff) {\n"
    "    exponent = 0xfe; fraction = 0x7fffffu;\n"
    "  } else {\n"
    "    fraction -= 0x800000u;\n"
    "  }\n"
    "  return (sign_bit ? 0x80000000u : 0u) |\n"
    "         (uint(exponent) << 23) | fraction;\n"
    "}\n"
    "uint nv20_shift_rz(uint value, int shift) {\n"
    "  if (shift >= 32) { return 0u; }\n"
    "  if (shift > 0) { return value >> uint(shift); }\n"
    "  if (shift <= -32) { return 0u; }\n"
    "  return value << uint(-shift);\n"
    "}\n"
    "uint nv20_mul_bits(uint a, uint b) {\n"
    "  uint ea = nv20_exp_bits(a), eb = nv20_exp_bits(b);\n"
    "  uint fa = ea == 0u ? 0u : 0x800000u | nv20_fract_bits(a);\n"
    "  uint fb = eb == 0u ? 0u : 0x800000u | nv20_fract_bits(b);\n"
    "  if (fa == 0u || fb == 0u) { return 0u; }\n"
    "  if (nv20_is_nan(a) || nv20_is_nan(b)) { return 0x7fffffffu; }\n"
    "  bool sign_bit = ((a ^ b) & 0x80000000u) != 0u;\n"
    "  if (nv20_is_inf(a) || nv20_is_inf(b)) {\n"
    "    return (sign_bit ? 0x80000000u : 0u) | 0x7f800000u;\n"
    "  }\n"
    "  int exponent = int(ea) + int(eb) - 126;\n"
    "  uint product_hi, product_lo;\n"
    "  umulExtended(fa, fb, product_hi, product_lo);\n"
    "  if ((product_hi & 0x8000u) == 0u) {\n"
    "    product_hi = (product_hi << 1) | (product_lo >> 31);\n"
    "    product_lo <<= 1;\n"
    "    exponent--;\n"
    "  }\n"
    "  uint fraction = (product_hi << 8) | (product_lo >> 24);\n"
    "  return nv20_make_finite(sign_bit, exponent, fraction);\n"
    "}\n"
    "uint nv20_sum_bits(uint a, uint b, uint c, uint d, int count) {\n"
    "  uint values[4] = uint[4](a, b, c, d);\n"
    "  bool negative_inf = false, positive_inf = false;\n"
    "  int exponent = 0;\n"
    "  for (int i = 0; i < count; i++) {\n"
    "    if (nv20_is_nan(values[i])) { return 0x7fffffffu; }\n"
    "    if (nv20_is_inf(values[i])) {\n"
    "      if ((values[i] & 0x80000000u) != 0u) { negative_inf = true; }\n"
    "      else { positive_inf = true; }\n"
    "    }\n"
    "    exponent = max(exponent, int(nv20_exp_bits(values[i])) + 2);\n"
    "  }\n"
    "  if (negative_inf && positive_inf) { return 0x7fffffffu; }\n"
    "  if (negative_inf || positive_inf) {\n"
    "    return (negative_inf ? 0x80000000u : 0u) | 0x7f800000u;\n"
    "  }\n"
    "  int accumulator = 0;\n"
    "  for (int i = 0; i < count; i++) {\n"
    "    int component_exp = int(nv20_exp_bits(values[i]));\n"
    "    uint component = nv20_fract_bits(values[i]);\n"
    "    if (component_exp != 0) { component |= 0x800000u; }\n"
    "    component = nv20_shift_rz(component,\n"
    "                                exponent - component_exp - 7);\n"
    "    accumulator += (values[i] & 0x80000000u) != 0u ?\n"
    "                   -int(component) : int(component);\n"
    "  }\n"
    "  if (accumulator == 0) { return 0u; }\n"
    "  bool sign_bit = accumulator < 0;\n"
    "  uint magnitude = uint(sign_bit ? -accumulator : accumulator);\n"
    "  int normalization = 30 - findMSB(magnitude);\n"
    "  magnitude <<= uint(normalization);\n"
    "  exponent -= normalization;\n"
    "  magnitude >>= 7;\n"
    "  return nv20_make_finite(sign_bit, exponent, magnitude);\n"
    "}\n"
    "uvec2 nv20_mul_wide(uint a, uint b) {\n"
    "  uint high_part, low_part;\n"
    "  umulExtended(a, b, high_part, low_part);\n"
    "  return uvec2(low_part, high_part);\n"
    "}\n"
    "uvec2 nv20_sub_wide(uvec2 a, uvec2 b) {\n"
    "  uint low_part = a.x - b.x;\n"
    "  return uvec2(low_part, a.y - b.y - uint(low_part > a.x));\n"
    "}\n"
    "uvec2 nv20_mul_wide_scalar(uvec2 a, uint b) {\n"
    "  uint high_part, low_part, carry, ignored;\n"
    "  umulExtended(a.x, b, carry, low_part);\n"
    "  umulExtended(a.y, b, ignored, high_part);\n"
    "  return uvec2(low_part, high_part + carry);\n"
    "}\n"
    "uint nv20_wide_shift_rz(uvec2 value, uint shift) {\n"
    "  return (value.y << (32u - shift)) | (value.x >> shift);\n"
    "}\n"
    "const uint nv20_rcp_lut[64] = uint[64](\n"
    "  0x7fu,0x7du,0x7bu,0x79u,0x78u,0x76u,0x74u,0x73u,\n"
    "  0x71u,0x6fu,0x6eu,0x6du,0x6bu,0x6au,0x68u,0x67u,\n"
    "  0x66u,0x65u,0x63u,0x62u,0x61u,0x60u,0x5fu,0x5eu,\n"
    "  0x5du,0x5cu,0x5bu,0x5au,0x59u,0x58u,0x57u,0x56u,\n"
    "  0x55u,0x54u,0x53u,0x52u,0x52u,0x51u,0x50u,0x4fu,\n"
    "  0x4eu,0x4eu,0x4du,0x4cu,0x4cu,0x4bu,0x4au,0x49u,\n"
    "  0x49u,0x48u,0x48u,0x47u,0x46u,0x46u,0x45u,0x45u,\n"
    "  0x44u,0x43u,0x43u,0x42u,0x42u,0x41u,0x41u,0x40u);\n"
    "uint nv20_rcp_bits(uint value, bool clamp_result) {\n"
    "  uint exponent_in = nv20_exp_bits(value);\n"
    "  uint fraction_in = nv20_fract_bits(value);\n"
    "  if (nv20_is_nan(value)) { return 0x7fffffffu; }\n"
    "  if (exponent_in == 0u && !clamp_result) {\n"
    "    return (value & 0x80000000u) | 0x7f800000u;\n"
    "  }\n"
    "  int exponent_out = 0xfe - int(exponent_in);\n"
    "  uint fraction_out = 0u;\n"
    "  if (fraction_in != 0u) {\n"
    "    uint normalized = fraction_in + 0x800000u;\n"
    "    uint slope = nv20_rcp_lut[(normalized >> 17) & 0x3fu];\n"
    "    uint stage1_term = 0x80000000u - slope * normalized;\n"
    "    uvec2 stage1_product = nv20_mul_wide(stage1_term, slope);\n"
    "    uint stage1 = nv20_wide_shift_rz(stage1_product, 24u);\n"
    "    uvec2 stage2_term = nv20_sub_wide(\n"
    "        uvec2(0u, 0x20u), nv20_mul_wide(stage1, normalized));\n"
    "    uvec2 stage2_product = nv20_mul_wide_scalar(stage2_term, stage1);\n"
    "    fraction_out = nv20_wide_shift_rz(stage2_product, 25u) -\n"
    "                   0x800000u;\n"
    "    exponent_out--;\n"
    "  }\n"
    "  if (clamp_result && exponent_out < 0x3f) {\n"
    "    exponent_out = 0x3f; fraction_out = 0u;\n"
    "  } else if (clamp_result && exponent_out >= 0xbf) {\n"
    "    exponent_out = 0xbf; fraction_out = 0u;\n"
    "  } else if (!clamp_result && exponent_out <= 0) {\n"
    "    exponent_out = 0; fraction_out = 0u;\n"
    "  }\n"
    "  return (value & 0x80000000u) | (uint(exponent_out) << 23) |\n"
    "         fraction_out;\n"
    "}\n"
    "float nv20_mul(float a, float b) {\n"
    "  return uintBitsToFloat(nv20_mul_bits(floatBitsToUint(a),\n"
    "                                       floatBitsToUint(b)));\n"
    "}\n"
    "float nv20_sum2(float a, float b) {\n"
    "  return uintBitsToFloat(nv20_sum_bits(floatBitsToUint(a),\n"
    "      floatBitsToUint(b), 0u, 0u, 2));\n"
    "}\n"
    "float nv20_sum3(float a, float b, float c) {\n"
    "  return uintBitsToFloat(nv20_sum_bits(floatBitsToUint(a),\n"
    "      floatBitsToUint(b), floatBitsToUint(c), 0u, 3));\n"
    "}\n"
    "float nv20_sum4(float a, float b, float c, float d) {\n"
    "  return uintBitsToFloat(nv20_sum_bits(floatBitsToUint(a),\n"
    "      floatBitsToUint(b), floatBitsToUint(c), floatBitsToUint(d), 4));\n"
    "}\n"
    "#endif\n"
    "\n"
    "#define MOV(dest, mask, src) dest.mask = _MOV(_in(src)).mask\n"
    "vec4 _MOV(vec4 src)\n"
    "{\n"
    "  return src;\n"
    "}\n"
    "\n"
    "#define MUL(dest, mask, src0, src1) dest.mask = _MUL(_in(src0), _in(src1)).mask\n"
    "vec4 _MUL(vec4 src0, vec4 src1)\n"
    "{\n"
    "#ifdef NV2A_NV20_ARITHMETIC\n"
    "  return vec4(nv20_mul(src0.x, src1.x), nv20_mul(src0.y, src1.y),\n"
    "              nv20_mul(src0.z, src1.z), nv20_mul(src0.w, src1.w));\n"
    "#else\n"
    // Unfortunately mix() falls victim to the same handling of exceptional
    // (inf/NaN) handling as a multiply, so per-component comparisons are used
    // to guarantee HW behavior (anything * 0 must == 0).
    "  vec4 zero_components = sign(NaNToOne(src0)) * sign(NaNToOne(src1));\n"
    "  vec4 ret = src0 * src1;\n"
    "  if (zero_components.x == 0.0) { ret.x = 0.0; }\n"
    "  if (zero_components.y == 0.0) { ret.y = 0.0; }\n"
    "  if (zero_components.z == 0.0) { ret.z = 0.0; }\n"
    "  if (zero_components.w == 0.0) { ret.w = 0.0; }\n"
    "  return ret;\n"
    "#endif\n"
    "}\n"
    "\n"
    "#define ADD(dest, mask, src0, src1) dest.mask = _ADD(_in(src0), _in(src1)).mask\n"
    "vec4 _ADD(vec4 src0, vec4 src1)\n"
    "{\n"
    "#ifdef NV2A_NV20_ARITHMETIC\n"
    "  return vec4(nv20_sum2(src0.x, src1.x),\n"
    "              nv20_sum2(src0.y, src1.y),\n"
    "              nv20_sum2(src0.z, src1.z),\n"
    "              nv20_sum2(src0.w, src1.w));\n"
    "#else\n"
    "  return src0 + src1;\n"
    "#endif\n"
    "}\n"
    "\n"
    "#define MAD(dest, mask, src0, src1, src2) dest.mask = _MAD(_in(src0), _in(src1), _in(src2)).mask\n"
    "vec4 _MAD(vec4 src0, vec4 src1, vec4 src2)\n"
    "{\n"
    "  return _ADD(_MUL(src0, src1), src2);\n"
    "}\n"
    "\n"
    "#define DP3(dest, mask, src0, src1) dest.mask = _DP3(_in(src0), _in(src1)).mask\n"
    "vec4 _DP3(vec4 src0, vec4 src1)\n"
    "{\n"
    "#ifdef NV2A_NV20_ARITHMETIC\n"
    "  float result = nv20_sum3(nv20_mul(src0.x, src1.x),\n"
    "      nv20_mul(src0.y, src1.y), nv20_mul(src0.z, src1.z));\n"
    "  return vec4(result);\n"
    "#else\n"
    "  return vec4(dot(src0.xyz, src1.xyz));\n"
    "#endif\n"
    "}\n"
    "\n"
    "#define DPH(dest, mask, src0, src1) dest.mask = _DPH(_in(src0), _in(src1)).mask\n"
    "vec4 _DPH(vec4 src0, vec4 src1)\n"
    "{\n"
    "#ifdef NV2A_NV20_ARITHMETIC\n"
    "  float result = nv20_sum4(nv20_mul(src0.x, src1.x),\n"
    "      nv20_mul(src0.y, src1.y), nv20_mul(src0.z, src1.z),\n"
    "      nv20_mul(1.0, src1.w));\n"
    "  return vec4(result);\n"
    "#else\n"
    "  return vec4(dot(vec4(src0.xyz, 1.0), src1));\n"
    "#endif\n"
    "}\n"
    "\n"
    "#define DP4(dest, mask, src0, src1) dest.mask = _DP4(_in(src0), _in(src1)).mask\n"
    "vec4 _DP4(vec4 src0, vec4 src1)\n"
    "{\n"
    "#ifdef NV2A_NV20_ARITHMETIC\n"
    "  float result = nv20_sum4(nv20_mul(src0.x, src1.x),\n"
    "      nv20_mul(src0.y, src1.y), nv20_mul(src0.z, src1.z),\n"
    "      nv20_mul(src0.w, src1.w));\n"
    "  return vec4(result);\n"
    "#else\n"
    "  return vec4(dot(src0, src1));\n"
    "#endif\n"
    "}\n"
    "\n"
    "#define DST(dest, mask, src0, src1) dest.mask = _DST(_in(src0), _in(src1)).mask\n"
    "vec4 _DST(vec4 src0, vec4 src1)\n"
    "{\n"
    "  return vec4(1.0,\n"
    "              src0.y * src1.y,\n"
    "              src0.z,\n"
    "              src1.w);\n"
    "}\n"
    "\n"
    "#define MIN(dest, mask, src0, src1) dest.mask = _MIN(_in(src0), _in(src1)).mask\n"
    "vec4 _MIN(vec4 src0, vec4 src1)\n"
    "{\n"
    "  return min(src0, src1);\n"
    "}\n"
    "\n"
    "#define MAX(dest, mask, src0, src1) dest.mask = _MAX(_in(src0), _in(src1)).mask\n"
    "vec4 _MAX(vec4 src0, vec4 src1)\n"
    "{\n"
    "  return max(src0, src1);\n"
    "}\n"
    "\n"
    "#define SLT(dest, mask, src0, src1) dest.mask = _SLT(_in(src0), _in(src1)).mask\n"
    "vec4 _SLT(vec4 src0, vec4 src1)\n"
    "{\n"
    "  return vec4(lessThan(src0, src1));\n"
    "}\n"
    "\n"
    "#define ARL(dest, src) dest = _ARL(_in(src).x)\n"
    "int _ARL(float src)\n"
    "{\n"
    "  /* Xbox GPU does specify rounding, OpenGL doesn't; so we need a bias.\n"
    "   * Example: We probably want to floor 16.99.. to 17, not 16.\n"
    "   * Source of error (why we get 16.99.. instead of 17.0) is typically\n"
    "   * vertex-attributes being normalized from a byte value to float:\n"
    "   *   17 / 255 = 0.06666.. so is this 0.06667 (ceil) or 0.06666 (floor)?\n"
    "   * Which value we get depends on the host GPU.\n"
    "   * If we multiply these rounded values by 255 later, we get:\n"
    "   *   17.00 (ARL result = 17) or 16.99 (ARL result = 16).\n"
    "   * We assume the intend was to get 17, so we add our bias to fix it. */\n"
    "  return int(floor(src + 0.001));\n"
    "}\n"
    "\n"
    "#define SGE(dest, mask, src0, src1) dest.mask = _SGE(_in(src0), _in(src1)).mask\n"
    "vec4 _SGE(vec4 src0, vec4 src1)\n"
    "{\n"
    "  return vec4(greaterThanEqual(src0, src1));\n"
    "}\n"
    "\n"
    "#define RCP(dest, mask, src) dest.mask = _RCP(_in(src).x).mask\n"
    "vec4 _RCP(float src)\n"
    "{\n"
    "#ifdef NV2A_NV20_ARITHMETIC\n"
    "  return vec4(uintBitsToFloat(nv20_rcp_bits(floatBitsToUint(src),\n"
    "                                            false)));\n"
    "#else\n"
    "  return vec4(1.0 / src);\n"
    "#endif\n"
    "}\n"
    "\n"
    "#define RCC(dest, mask, src) dest.mask = _RCC(_in(src).x).mask\n"
    "vec4 _RCC(float src)\n"
    "{\n"
    "#ifdef NV2A_NV20_ARITHMETIC\n"
    "  return vec4(uintBitsToFloat(nv20_rcp_bits(floatBitsToUint(src),\n"
    "                                            true)));\n"
    "#else\n"
    "  float t = clampAwayZeroInf(1.0 / src);\n"
    "  return vec4(t);\n"
    "#endif\n"
    "}\n"
    "\n"
    "#define RSQ(dest, mask, src) dest.mask = _RSQ(_in(src).x).mask\n"
    "vec4 _RSQ(float src)\n"
    "{\n"
    "  if (src == 0.0) { return vec4(INFINITY); }\n"
    "  if (isinf(src)) { return vec4(0.0); }\n"
    "  return vec4(inversesqrt(abs(src)));\n"
    "}\n"
    "\n"
    "#define EXP(dest, mask, src) dest.mask = _EXP(_in(src).x).mask\n"
    "vec4 _EXP(float src)\n"
    "{\n"
    "  vec4 result;\n"
    "  result.x = exp2(floor(src));\n"
    "  result.y = src - floor(src);\n"
    "  result.z = exp2(src);\n"
    "  result.w = 1.0;\n"
    "  return result;\n"
    "}\n"
    "\n"
    "#define LOG(dest, mask, src) dest.mask = _LOG(_in(src).x).mask\n"
    "vec4 _LOG(float src)\n"
    "{\n"
    "  float tmp = abs(src);\n"
    "  if (tmp == 0.0) { return vec4(-INFINITY, 1.0f, -INFINITY, 1.0f); }\n"
    "  vec4 result;\n"
    "  result.x = floor(log2(tmp));\n"
    "  result.y = tmp / exp2(floor(log2(tmp)));\n"
    "  result.z = log2(tmp);\n"
    "  result.w = 1.0;\n"
    "  return result;\n"
    "}\n"
    "\n"
    "#define LIT(dest, mask, src) dest.mask = _LIT(_in(src)).mask\n"
    "vec4 _LIT(vec4 src)\n"
    "{\n"
    "  vec4 s = src;\n"
    "  float epsilon = 1.0 / 256.0;\n"
    "  s.w = clamp(s.w, -(128.0 - epsilon), 128.0 - epsilon);\n"
    "  s.x = max(s.x, 0.0);\n"
    "  s.y = max(s.y, 0.0);\n"
    "  vec4 t = vec4(1.0, 0.0, 0.0, 1.0);\n"
    "  t.y = s.x;\n"
#if 1
    "  t.z = (s.x > 0.0) ? exp2(s.w * log2(s.y)) : 0.0;\n"
#else
    "  t.z = (s.x > 0.0) ? pow(s.y, s.w) : 0.0;\n"
#endif
    "  return t;\n"
    "}\n";

void pgraph_glsl_gen_vsh_prog(uint16_t version, const uint32_t *tokens,
                              unsigned int length,
                              bool nv20_vertex_arithmetic, MString *header,
                              MString *body)
{
    if (nv20_vertex_arithmetic) {
        mstring_append(header, "#define NV2A_NV20_ARITHMETIC 1\n");
    }
    mstring_append(header, vsh_header);

    bool has_final = false;
    int slot;

    for (slot=0; slot < length; slot++) {
        const uint32_t* cur_token = &tokens[slot * VSH_TOKEN_SIZE];
        MString *token_str = decode_token(cur_token);
        mstring_append_fmt(body,
                           "  /* Slot %d: 0x%08X 0x%08X 0x%08X 0x%08X */\n"
                           "  %s\n",
                           slot, cur_token[0], cur_token[1], cur_token[2],
                           cur_token[3], mstring_get_str(token_str));
        mstring_unref(token_str);

        if (vsh_get_field(cur_token, FLD_FINAL)) {
            has_final = true;
            break;
        }
    }
    assert(has_final);

    mstring_append(body,
        /* The shaders leave the result in screen space, while OpenGL expects it
         * in clip space.
         */
        "  oPos.xy = roundScreenCoords(oPos.xy);\n"
        "  oPos.w = clampAwayZeroInf(oPos.w);\n"
        "  vec4 vtxPos = oPos;\n"
        "  oPos.xy = (2.0f * oPos.xy - surfaceSize) / surfaceSize;\n"
        "  oPos.z = oPos.z / clipRange.y;\n"

        /* Undo perspective divide by w.
         * Note that games may also have vertex shaders that do
         * not divide by w (such as 2D-graphics menus or overlays), but since
         * OpenGL will later on divide by the same w, we get back the same
         * screen space coordinates (perhaps with some loss of floating point
         * precision, though.)
         */
        "  oPos.xyz *= oPos.w;\n"
    );
}
