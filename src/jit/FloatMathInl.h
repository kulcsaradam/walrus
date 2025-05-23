/*
 * Copyright (c) 2022-present Samsung Electronics Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Only included by jit-backend.cc */

void emitSelect(sljit_compiler* compiler, Instruction* instr, sljit_s32 type);

using f32Function2Param = std::add_pointer<sljit_f32(sljit_f32, sljit_f32)>::type;
using f64Function2Param = std::add_pointer<sljit_f64(sljit_f64, sljit_f64)>::type;
using f32Function1Param = std::add_pointer<sljit_f32(sljit_f32)>::type;
using f64Function1Param = std::add_pointer<sljit_f64(sljit_f64)>::type;

#define MOVE_FROM_FREG(compiler, movOp, arg, argw, sourceReg)            \
    if ((sourceReg) != (arg)) {                                          \
        sljit_emit_fop1(compiler, movOp, (arg), (argw), (sourceReg), 0); \
    }
#define MOVE_TO_FREG(compiler, movOp, targetReg, arg, argw)              \
    if ((targetReg) != (arg)) {                                          \
        sljit_emit_fop1(compiler, movOp, (targetReg), 0, (arg), (argw)); \
    }

static void floatOperandToArg(sljit_compiler* compiler, Operand* operand, JITArg& arg, sljit_s32 srcReg)
{
    if (VARIABLE_TYPE(*operand) != Instruction::ConstPtr) {
        arg.set(operand);
        return;
    }

    arg.arg = srcReg;
    arg.argw = 0;

    Instruction* instr = VARIABLE_GET_IMM(*operand);
    ASSERT(srcReg != 0);

    if (instr->opcode() == ByteCode::Const32Opcode) {
        union {
            uint32_t value;
            sljit_f32 number;
        } u;

        u.value = reinterpret_cast<Const32*>(instr->byteCode())->value();
        sljit_emit_fset32(compiler, srcReg, u.number);
        return;
    }

    union {
        uint64_t value;
        sljit_f64 number;
    } u;

    u.value = reinterpret_cast<Const64*>(instr->byteCode())->value();
    sljit_emit_fset64(compiler, srcReg, u.number);
}

static void emitMoveFloat(sljit_compiler* compiler, Instruction* instr)
{
    Operand* operands = instr->operands();

    JITArg src;
    JITArg dst(operands + 1);
    sljit_s32 tmpReg = GET_TARGET_REG(dst.arg, SLJIT_TMP_DEST_FREG);

    floatOperandToArg(compiler, operands + 0, src, tmpReg);

    sljit_s32 op = (instr->opcode() == ByteCode::MoveF32Opcode) ? SLJIT_MOV_F32 : SLJIT_MOV_F64;

    // Immediate to register case has already been handled.
    if (dst.arg != src.arg || dst.argw != src.argw) {
        sljit_emit_fop1(compiler, op, dst.arg, dst.argw, src.arg, src.argw);
    }
}

static void emitInitFR0FR1(sljit_compiler* compiler, sljit_s32 movOp, JITArg* params)
{
    if (params[1].arg != SLJIT_FR0) {
        MOVE_TO_FREG(compiler, movOp, SLJIT_FR0, params[0].arg, params[0].argw);
        MOVE_TO_FREG(compiler, movOp, SLJIT_FR1, params[1].arg, params[1].argw);
        return;
    }

    if (params[0].arg != SLJIT_FR1) {
        sljit_emit_fop1(compiler, movOp, SLJIT_FR1, 0, SLJIT_FR0, 0);
        MOVE_TO_FREG(compiler, movOp, SLJIT_FR0, params[0].arg, params[0].argw);
        return;
    }

    // Swap arguments.
    sljit_emit_fop1(compiler, movOp, SLJIT_TMP_DEST_FREG, 0, SLJIT_FR0, 0);
    sljit_emit_fop1(compiler, movOp, SLJIT_FR0, 0, SLJIT_FR1, 0);
    sljit_emit_fop1(compiler, movOp, SLJIT_FR1, 0, SLJIT_TMP_DEST_FREG, 0);
}

// Float operations.
// TODO Canonical NaN
static sljit_f32 floatFloor(sljit_f32 operand)
{
    if (std::isnan(operand)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    return std::floor(operand);
}

static sljit_f64 floatFloor(sljit_f64 operand)
{
    if (std::isnan(operand)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    return std::floor(operand);
}

static sljit_f32 floatCeil(sljit_f32 operand)
{
    if (std::isnan(operand)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    return std::ceil(operand);
}

static sljit_f64 floatCeil(sljit_f64 operand)
{
    if (std::isnan(operand)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    return std::ceil(operand);
}

static sljit_f32 floatTrunc(sljit_f32 operand)
{
    if (std::isnan(operand)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    return std::trunc(operand);
}

static sljit_f64 floatTrunc(sljit_f64 operand)
{
    if (std::isnan(operand)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    return std::trunc(operand);
}

static sljit_f32 floatNearest(sljit_f32 val)
{
    return std::nearbyint(val);
}

static sljit_f64 floatNearest(sljit_f64 val)
{
    return std::nearbyint(val);
}

static sljit_f32 floatMin(sljit_f32 lhs, sljit_f32 rhs)
{
    if (SLJIT_UNLIKELY(std::isnan(lhs) || std::isnan(rhs))) {
        return std::numeric_limits<float>::quiet_NaN();
    } else if (SLJIT_UNLIKELY(lhs == 0 && rhs == 0)) {
        return std::signbit(lhs) ? lhs : rhs;
    }

    return std::min(lhs, rhs);
}

static sljit_f64 floatMin(sljit_f64 lhs, sljit_f64 rhs)
{
    if (SLJIT_UNLIKELY(std::isnan(lhs) || std::isnan(rhs))) {
        return std::numeric_limits<double>::quiet_NaN();
    } else if (SLJIT_UNLIKELY(lhs == 0 && rhs == 0)) {
        return std::signbit(lhs) ? lhs : rhs;
    }

    return std::min(lhs, rhs);
}

static sljit_f32 floatMax(sljit_f32 lhs, sljit_f32 rhs)
{
    if (SLJIT_UNLIKELY(std::isnan(lhs) || std::isnan(rhs))) {
        return std::numeric_limits<float>::quiet_NaN();
    } else if (SLJIT_UNLIKELY(lhs == 0 && rhs == 0)) {
        return std::signbit(lhs) ? rhs : lhs;
    }

    return std::max(lhs, rhs);
}

static sljit_f64 floatMax(sljit_f64 lhs, sljit_f64 rhs)
{
    if (SLJIT_UNLIKELY(std::isnan(lhs) || std::isnan(rhs))) {
        return std::numeric_limits<double>::quiet_NaN();
    } else if (SLJIT_UNLIKELY(lhs == 0 && rhs == 0)) {
        return std::signbit(lhs) ? rhs : lhs;
    }

    return std::max(lhs, rhs);
}

static void emitFloatBinary(sljit_compiler* compiler, Instruction* instr)
{
    Operand* operands = instr->operands();
    JITArg args[3];

    ASSERT(instr->paramCount() == 2 && instr->resultCount() == 1);

    floatOperandToArg(compiler, operands, args[0], instr->requiredReg(0));
    floatOperandToArg(compiler, operands + 1, args[1], instr->requiredReg(1));
    floatOperandToArg(compiler, operands + 2, args[2], 0);

    sljit_s32 opcode = SLJIT_NOP;
    sljit_s32 dstReg;
    f32Function2Param f32Func = nullptr;
    f64Function2Param f64Func = nullptr;

    switch (instr->opcode()) {
    case ByteCode::F32AddOpcode:
        opcode = SLJIT_ADD_F32;
        break;
    case ByteCode::F32SubOpcode:
        opcode = SLJIT_SUB_F32;
        break;
    case ByteCode::F32MulOpcode:
        opcode = SLJIT_MUL_F32;
        break;
    case ByteCode::F32DivOpcode:
        opcode = SLJIT_DIV_F32;
        break;
    case ByteCode::F32MaxOpcode:
        f32Func = floatMax;
        break;
    case ByteCode::F32MinOpcode:
        f32Func = floatMin;
        break;
    case ByteCode::F64AddOpcode:
        opcode = SLJIT_ADD_F64;
        break;
    case ByteCode::F64SubOpcode:
        opcode = SLJIT_SUB_F64;
        break;
    case ByteCode::F64MulOpcode:
        opcode = SLJIT_MUL_F64;
        break;
    case ByteCode::F64DivOpcode:
        opcode = SLJIT_DIV_F64;
        break;
    case ByteCode::F64MaxOpcode:
        f64Func = floatMax;
        break;
    case ByteCode::F64MinOpcode:
        f64Func = floatMin;
        break;
    case ByteCode::F32CopysignOpcode:
    case ByteCode::F64CopysignOpcode:
        dstReg = GET_TARGET_REG(args[2].arg, instr->requiredReg(2));
        opcode = instr->opcode() == ByteCode::F32CopysignOpcode ? SLJIT_COPYSIGN_F32 : SLJIT_COPYSIGN_F64;
        sljit_emit_fop2r(compiler, opcode, dstReg, args[0].arg, args[0].argw, args[1].arg, args[1].argw);
        opcode = instr->opcode() == ByteCode::F32CopysignOpcode ? SLJIT_MOV_F32 : SLJIT_MOV_F64;
        MOVE_FROM_FREG(compiler, opcode, args[2].arg, args[2].argw, instr->requiredReg(2));
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    if (opcode != SLJIT_NOP) {
        ASSERT(!(instr->info() & Instruction::kIsCallback));
        sljit_emit_fop2(compiler, opcode, args[2].arg, args[2].argw, args[0].arg, args[0].argw, args[1].arg, args[1].argw);
        return;
    }

    ASSERT(instr->info() & Instruction::kIsCallback);

    if (f32Func) {
        emitInitFR0FR1(compiler, SLJIT_MOV_F32, args);
        sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS2(F32, F32, F32), SLJIT_IMM, GET_FUNC_ADDR(sljit_sw, f32Func));
        MOVE_FROM_FREG(compiler, SLJIT_MOV_F32, args[2].arg, args[2].argw, SLJIT_FR0);
        return;
    }

    emitInitFR0FR1(compiler, SLJIT_MOV_F64, args);
    sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS2(F64, F64, F64), SLJIT_IMM, GET_FUNC_ADDR(sljit_sw, f64Func));
    MOVE_FROM_FREG(compiler, SLJIT_MOV_F64, args[2].arg, args[2].argw, SLJIT_FR0);
}

static void emitFloatUnary(sljit_compiler* compiler, Instruction* instr)
{
    Operand* operands = instr->operands();
    JITArg args[2];

    ASSERT(instr->paramCount() == 1 && instr->resultCount() == 1);

    floatOperandToArg(compiler, operands, args[0], instr->requiredReg(0));
    floatOperandToArg(compiler, operands + 1, args[1], 0);

    sljit_s32 opcode;
    f32Function1Param f32Func = nullptr;
    f64Function1Param f64Func = nullptr;

    switch (instr->opcode()) {
    case ByteCode::F32CeilOpcode:
        f32Func = floatCeil;
        break;
    case ByteCode::F32FloorOpcode:
        f32Func = floatFloor;
        break;
    case ByteCode::F32TruncOpcode:
        f32Func = floatTrunc;
        break;
    case ByteCode::F32NearestOpcode:
        f32Func = floatNearest;
        break;
    case ByteCode::F32SqrtOpcode:
        f32Func = sqrtf;
        break;
    case ByteCode::F32NegOpcode:
        opcode = SLJIT_NEG_F32;
        break;
    case ByteCode::F32AbsOpcode:
        opcode = SLJIT_ABS_F32;
        break;
    case ByteCode::F32DemoteF64Opcode:
        opcode = SLJIT_CONV_F32_FROM_F64;
        break;
    case ByteCode::F64CeilOpcode:
        f64Func = floatCeil;
        break;
    case ByteCode::F64FloorOpcode:
        f64Func = floatFloor;
        break;
    case ByteCode::F64TruncOpcode:
        f64Func = floatTrunc;
        break;
    case ByteCode::F64NearestOpcode:
        f64Func = floatNearest;
        break;
    case ByteCode::F64SqrtOpcode:
        f64Func = sqrt;
        break;
    case ByteCode::F64NegOpcode:
        opcode = SLJIT_NEG_F64;
        break;
    case ByteCode::F64AbsOpcode:
        opcode = SLJIT_ABS_F64;
        break;
    case ByteCode::F64PromoteF32Opcode:
        opcode = SLJIT_CONV_F64_FROM_F32;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    if (f32Func) {
        ASSERT(instr->info() & Instruction::kIsCallback);
        MOVE_TO_FREG(compiler, SLJIT_MOV_F32, SLJIT_FR0, args[0].arg, args[0].argw);
        sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1(F32, F32), SLJIT_IMM, GET_FUNC_ADDR(sljit_sw, f32Func));
        MOVE_FROM_FREG(compiler, SLJIT_MOV_F32, args[1].arg, args[1].argw, SLJIT_FR0);
    } else if (f64Func) {
        ASSERT(instr->info() & Instruction::kIsCallback);
        MOVE_TO_FREG(compiler, SLJIT_MOV_F64, SLJIT_FR0, args[0].arg, args[0].argw);
        sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1(F64, F64), SLJIT_IMM, GET_FUNC_ADDR(sljit_sw, f64Func));
        MOVE_FROM_FREG(compiler, SLJIT_MOV_F64, args[1].arg, args[1].argw, SLJIT_FR0);
    } else {
        ASSERT(!(instr->info() & Instruction::kIsCallback));
        sljit_emit_fop1(compiler, opcode, args[1].arg, args[1].argw, args[0].arg, args[0].argw);
    }
}

static void emitFloatSelect(sljit_compiler* compiler, Instruction* instr, sljit_s32 type)
{
    Operand* operands = instr->operands();
    bool is32 = reinterpret_cast<Select*>(instr->byteCode())->valueSize() == 4;
    sljit_s32 movOpcode = is32 ? SLJIT_MOV_F32 : SLJIT_MOV_F64;
    JITArg args[3];

    floatOperandToArg(compiler, operands + 3, args[2], 0);
    floatOperandToArg(compiler, operands + 0, args[0], instr->requiredReg(0));
    floatOperandToArg(compiler, operands + 1, args[1], instr->requiredReg(1));

    if (type == -1) {
        JITArg cond(operands + 2);

        sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z, cond.arg, cond.argw, SLJIT_IMM, 0);

        type = SLJIT_NOT_ZERO;
    }

    sljit_s32 targetReg = GET_TARGET_REG(args[2].arg, SLJIT_TMP_DEST_FREG);
    sljit_s32 baseReg = 0;

    if (args[1].arg == targetReg) {
        baseReg = 1;
    } else {
        type ^= 1;
    }

    if (!SLJIT_IS_REG(args[baseReg].arg)) {
        sljit_emit_fop1(compiler, movOpcode, targetReg, 0, args[baseReg].arg, args[baseReg].argw);
        args[baseReg].arg = targetReg;
    }

    if (is32) {
        type |= SLJIT_32;
    }

    sljit_s32 otherReg = baseReg ^ 0x1;
    sljit_emit_fselect(compiler, type, targetReg, args[otherReg].arg, args[otherReg].argw, args[baseReg].arg);
    MOVE_FROM_FREG(compiler, movOpcode, args[2].arg, args[2].argw, targetReg);
}

static bool emitFloatCompare(sljit_compiler* compiler, Instruction* instr)
{
    Operand* operand = instr->operands();
    sljit_s32 opcode, type;
    JITArg params[3];

    ASSERT(instr->paramCount() == 2);

    for (uint32_t i = 0; i < 2; ++i, operand++) {
        floatOperandToArg(compiler, operand, params[i], instr->requiredReg(i));
    }

    switch (instr->opcode()) {
    case ByteCode::F32EqOpcode:
        opcode = SLJIT_CMP_F32 | SLJIT_SET_ORDERED_EQUAL;
        type = SLJIT_ORDERED_EQUAL;
        break;
    case ByteCode::F32NeOpcode:
        opcode = SLJIT_CMP_F32 | SLJIT_SET_UNORDERED_OR_NOT_EQUAL;
        type = SLJIT_UNORDERED_OR_NOT_EQUAL;
        break;
    case ByteCode::F32LtOpcode:
        opcode = SLJIT_CMP_F32 | SLJIT_SET_ORDERED_LESS;
        type = SLJIT_ORDERED_LESS;
        break;
    case ByteCode::F32LeOpcode:
        opcode = SLJIT_CMP_F32 | SLJIT_SET_ORDERED_LESS_EQUAL;
        type = SLJIT_ORDERED_LESS_EQUAL;
        break;
    case ByteCode::F32GtOpcode:
        opcode = SLJIT_CMP_F32 | SLJIT_SET_ORDERED_GREATER;
        type = SLJIT_ORDERED_GREATER;
        break;
    case ByteCode::F32GeOpcode:
        opcode = SLJIT_CMP_F32 | SLJIT_SET_ORDERED_GREATER_EQUAL;
        type = SLJIT_ORDERED_GREATER_EQUAL;
        break;
    case ByteCode::F64EqOpcode:
        opcode = SLJIT_CMP_F64 | SLJIT_SET_ORDERED_EQUAL;
        type = SLJIT_ORDERED_EQUAL;
        break;
    case ByteCode::F64NeOpcode:
        opcode = SLJIT_CMP_F64 | SLJIT_SET_UNORDERED_OR_NOT_EQUAL;
        type = SLJIT_UNORDERED_OR_NOT_EQUAL;
        break;
    case ByteCode::F64LtOpcode:
        opcode = SLJIT_CMP_F64 | SLJIT_SET_ORDERED_LESS;
        type = SLJIT_ORDERED_LESS;
        break;
    case ByteCode::F64LeOpcode:
        opcode = SLJIT_CMP_F64 | SLJIT_SET_ORDERED_LESS_EQUAL;
        type = SLJIT_ORDERED_LESS_EQUAL;
        break;
    case ByteCode::F64GtOpcode:
        opcode = SLJIT_CMP_F64 | SLJIT_SET_ORDERED_GREATER;
        type = SLJIT_ORDERED_GREATER;
        break;
    case ByteCode::F64GeOpcode:
        opcode = SLJIT_CMP_F64 | SLJIT_SET_ORDERED_GREATER_EQUAL;
        type = SLJIT_ORDERED_GREATER_EQUAL;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    Instruction* nextInstr = nullptr;

    ASSERT(instr->next() != nullptr);

    if (instr->info() & Instruction::kIsMergeCompare) {
        nextInstr = instr->next()->asInstruction();

        if (nextInstr->opcode() != ByteCode::SelectOpcode) {
            ASSERT(nextInstr->opcode() == ByteCode::JumpIfTrueOpcode || nextInstr->opcode() == ByteCode::JumpIfFalseOpcode);

            if (nextInstr->opcode() == ByteCode::JumpIfFalseOpcode) {
                type ^= 0x1;
            }

            type |= (opcode & SLJIT_32);

            sljit_jump* jump = sljit_emit_fcmp(compiler, type, params[0].arg, params[0].argw,
                                               params[1].arg, params[1].argw);
            nextInstr->asExtended()->value().targetLabel->jumpFrom(jump);
            return true;
        }
    }

    sljit_emit_fop1(compiler, opcode, params[0].arg, params[0].argw,
                    params[1].arg, params[1].argw);

    if (nextInstr != nullptr) {
        emitSelect(compiler, nextInstr, type);
        return true;
    }

    params[0].set(operand);
    sljit_emit_op_flags(compiler, SLJIT_MOV32, params[0].arg, params[0].argw, type);
    return false;
}
