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
#include "Walrus.h"

#include "parser/WASMParser.h"
#include "interpreter/ByteCode.h"
#include "runtime/Store.h"
#include "runtime/Module.h"

#include "wabt/walrus/binary-reader-walrus.h"

namespace wabt {

enum class WASMOpcode : size_t {
#define WABT_OPCODE(rtype, type1, type2, type3, memSize, prefix, code, name, \
                    text, decomp)                                            \
    name##Opcode,
#include "parser/opcode.def"
#undef WABT_OPCODE
    OpcodeKindEnd,
};

struct WASMCodeInfo {
    enum CodeType { ___,
                    I32,
                    I64,
                    F32,
                    F64,
                    V128 };
    WASMOpcode m_code;
    CodeType m_resultType;
    CodeType m_paramTypes[3];
    const char* m_name;

    size_t stackShrinkSize() const
    {
        ASSERT(m_code != WASMOpcode::OpcodeKindEnd);
        return codeTypeToMemorySize(m_paramTypes[0]) + codeTypeToMemorySize(m_paramTypes[1]) + codeTypeToMemorySize(m_paramTypes[2]);
    }

    size_t stackGrowSize() const
    {
        ASSERT(m_code != WASMOpcode::OpcodeKindEnd);
        return codeTypeToMemorySize(m_resultType);
    }

    static size_t codeTypeToMemorySize(CodeType tp)
    {
        switch (tp) {
        case I32:
            return Walrus::stackAllocatedSize<int32_t>();
        case F32:
            return Walrus::stackAllocatedSize<float>();
        case I64:
            return Walrus::stackAllocatedSize<int64_t>();
        case F64:
            return Walrus::stackAllocatedSize<double>();
        case V128:
            return 16;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return 0;
        }
    }

    static Walrus::Value::Type codeTypeToValueType(CodeType tp)
    {
        switch (tp) {
        case I32:
            return Walrus::Value::Type::I32;
        case F32:
            return Walrus::Value::Type::F32;
        case I64:
            return Walrus::Value::Type::I64;
        case F64:
            return Walrus::Value::Type::F64;
        case V128:
            return Walrus::Value::Type::V128;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return Walrus::Value::Type::Void;
        }
    }
};

WASMCodeInfo g_wasmCodeInfo[static_cast<size_t>(WASMOpcode::OpcodeKindEnd)] = {
#define WABT_OPCODE(rtype, type1, type2, type3, memSize, prefix, code, name, \
                    text, decomp)                                            \
    { WASMOpcode::name##Opcode,                                              \
      WASMCodeInfo::rtype,                                                   \
      { WASMCodeInfo::type1, WASMCodeInfo::type2, WASMCodeInfo::type3 },     \
      text },
#include "parser/opcode.def"
#undef WABT_OPCODE
};

static Walrus::Value::Type toValueKind(Type type)
{
    switch (type) {
    case Type::I32:
        return Walrus::Value::Type::I32;
    case Type::I64:
        return Walrus::Value::Type::I64;
    case Type::F32:
        return Walrus::Value::Type::F32;
    case Type::F64:
        return Walrus::Value::Type::F64;
    case Type::V128:
        return Walrus::Value::Type::V128;
    case Type::FuncRef:
        return Walrus::Value::Type::FuncRef;
    case Type::ExternRef:
        return Walrus::Value::Type::ExternRef;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static Walrus::SegmentMode toSegmentMode(uint8_t flags)
{
    enum SegmentFlags : uint8_t {
        SegFlagsNone = 0,
        SegPassive = 1, // bit 0: Is passive
        SegExplicitIndex = 2, // bit 1: Has explict index (Implies table 0 if absent)
        SegDeclared = 3, // Only used for declared segments
        SegUseElemExprs = 4, // bit 2: Is elemexpr (Or else index sequence)
        SegFlagMax = (SegUseElemExprs << 1) - 1, // All bits set.
    };

    if ((flags & SegDeclared) == SegDeclared) {
        return Walrus::SegmentMode::Declared;
    } else if ((flags & SegPassive) == SegPassive) {
        return Walrus::SegmentMode::Passive;
    } else {
        return Walrus::SegmentMode::Active;
    }
}

class WASMBinaryReader : public wabt::WASMBinaryReaderDelegate {
private:
    struct VMStackInfo {
    public:
        VMStackInfo(WASMBinaryReader& reader, Walrus::Value::Type valueType, size_t position, size_t nonOptimizedPosition, size_t localIndex)
            : m_reader(reader)
            , m_valueType(valueType)
            , m_position(position)
            , m_nonOptimizedPosition(nonOptimizedPosition)
            , m_localIndex(localIndex)
        {
        }

        VMStackInfo(const VMStackInfo& src)
            : m_reader(src.m_reader)
            , m_valueType(src.m_valueType)
            , m_position(src.m_position)
            , m_nonOptimizedPosition(src.m_nonOptimizedPosition)
            , m_localIndex(src.m_localIndex)
        {
        }

        ~VMStackInfo()
        {
        }

        const VMStackInfo& operator=(const VMStackInfo& src)
        {
            m_reader = src.m_reader;
            m_valueType = src.m_valueType;
            m_position = src.m_position;
            m_nonOptimizedPosition = src.m_nonOptimizedPosition;
            m_localIndex = src.m_localIndex;
            return *this;
        }

        bool hasValidLocalIndex() const
        {
            return m_localIndex != std::numeric_limits<size_t>::max();
        }

        void clearLocalIndex()
        {
            m_localIndex = std::numeric_limits<size_t>::max();
        }

        size_t position() const
        {
            return m_position;
        }

        void setPosition(size_t position)
        {
            m_position = position;
        }

        Walrus::Value::Type valueType() const
        {
            return m_valueType;
        }

        size_t stackAllocatedSize() const
        {
            return Walrus::valueStackAllocatedSize(m_valueType);
        }

        size_t nonOptimizedPosition() const
        {
            return m_nonOptimizedPosition;
        }

        size_t localIndex() const
        {
            return m_localIndex;
        }

    private:
        WASMBinaryReader& m_reader;
        Walrus::Value::Type m_valueType;
        size_t m_position; // effective position (local values will have different position)
        size_t m_nonOptimizedPosition; // non-optimized position (same with m_functionStackSizeSoFar)
        size_t m_localIndex;
    };

    struct BlockInfo {
        enum BlockType {
            IfElse,
            Loop,
            Block,
            TryCatch,
        };
        BlockType m_blockType;
        Type m_returnValueType;
        size_t m_position;
        std::vector<VMStackInfo> m_vmStack;
        uint32_t m_functionStackSizeSoFar;
        bool m_shouldRestoreVMStackAtEnd;
        bool m_byteCodeGenerationStopped;

        static_assert(sizeof(Walrus::JumpIfTrue) == sizeof(Walrus::JumpIfFalse), "");
        struct JumpToEndBrInfo {
            enum JumpToEndType {
                IsJump,
                IsJumpIf,
                IsBrTable,
            };

            JumpToEndType m_type;
            size_t m_position;
        };

        std::vector<JumpToEndBrInfo> m_jumpToEndBrInfo;

        BlockInfo(BlockType type, Type returnValueType, WASMBinaryReader& binaryReader)
            : m_blockType(type)
            , m_returnValueType(returnValueType)
            , m_position(0)
            , m_functionStackSizeSoFar(binaryReader.m_functionStackSizeSoFar)
            , m_shouldRestoreVMStackAtEnd(false)
            , m_byteCodeGenerationStopped(false)
        {
            if (returnValueType.IsIndex() && binaryReader.m_result.m_functionTypes[returnValueType]->param().size()) {
                // record parameter positions
                auto& param = binaryReader.m_result.m_functionTypes[returnValueType]->param();
                auto endIter = binaryReader.m_vmStack.rbegin() + param.size();
                auto iter = binaryReader.m_vmStack.rbegin();
                while (iter != endIter) {
                    if (iter->hasValidLocalIndex()) {
                        binaryReader.generateMoveCodeIfNeeds(iter->position(), iter->nonOptimizedPosition(), iter->valueType());
                        iter->setPosition(iter->nonOptimizedPosition());
                        if (binaryReader.m_inPreprocess) {
                            size_t pos = *binaryReader.m_readerOffsetPointer;
                            auto localVariableIter = binaryReader.m_localVariableUsage.rbegin();
                            while (true) {
                                ASSERT(localVariableIter != binaryReader.m_localVariableUsage.rend());
                                if (localVariableIter->m_localIndex == iter->localIndex()
                                    && localVariableIter->m_endPosition == std::numeric_limits<size_t>::max()) {
                                    localVariableIter->m_endPosition = *binaryReader.m_readerOffsetPointer;
                                    break;
                                }
                                localVariableIter++;
                            }
                        }
                        iter->clearLocalIndex();
                    }
                    iter++;
                }
            }

            m_vmStack = binaryReader.m_vmStack;
            m_position = binaryReader.m_currentFunction->currentByteCodeSize();
        }
    };

    size_t* m_readerOffsetPointer;
    size_t m_codeStartOffset;

    struct LocalVariableUsage {
        size_t m_localIndex;
        size_t m_startPosition;
        size_t m_endPosition;
        size_t m_pushCount;
        bool m_hasWriteUsage;

        LocalVariableUsage(size_t localIndex, size_t startPosition, size_t pushCount)
            : m_localIndex(localIndex)
            , m_startPosition(startPosition)
            , m_endPosition(std::numeric_limits<size_t>::max())
            , m_pushCount(pushCount)
            , m_hasWriteUsage(false)
        {
        }
    };

    LocalVariableUsage& findNearestUsage(size_t localIndex)
    {
        size_t pos = *m_readerOffsetPointer;
        auto iter = m_localVariableUsage.rbegin();
        while (true) {
            ASSERT(iter != m_localVariableUsage.rend());
            if (iter->m_localIndex == localIndex) {
                if (iter->m_startPosition <= pos) {
                    return *iter;
                }
            }
            iter++;
        }
        ASSERT_NOT_REACHED();
    }

    bool m_inPreprocess;
    std::vector<LocalVariableUsage> m_localVariableUsage;

    Walrus::ModuleFunction* m_currentFunction;
    Walrus::FunctionType* m_currentFunctionType;
    uint32_t m_initialFunctionStackSize;
    uint32_t m_functionStackSizeSoFar;
    uint32_t m_lastByteCodePosition;

    std::vector<VMStackInfo> m_vmStack;
    std::vector<BlockInfo> m_blockInfo;
    struct CatchInfo {
        size_t m_tryCatchBlockDepth;
        size_t m_tryStart;
        size_t m_tryEnd;
        size_t m_catchStart;
        uint32_t m_tagIndex;
    };
    std::vector<CatchInfo> m_catchInfo;
    struct LocalInfo {
        Walrus::Value::Type m_valueType;
        LocalInfo(Walrus::Value::Type type)
            : m_valueType(type)
        {
        }
    };
    std::vector<LocalInfo> m_localInfo;

    Walrus::Vector<uint8_t, std::allocator<uint8_t>> m_memoryInitData;

    uint32_t m_elementTableIndex;
    Walrus::Optional<Walrus::ModuleFunction*> m_elementModuleFunction;
    Walrus::Vector<uint32_t, std::allocator<uint32_t>> m_elementFunctionIndex;
    Walrus::SegmentMode m_segmentMode;

    Walrus::WASMParsingResult m_result;

    virtual void OnSetOffsetAddress(size_t* ptr) override
    {
        m_readerOffsetPointer = ptr;
    }

    size_t pushVMStack(Walrus::Value::Type type)
    {
        auto pos = m_functionStackSizeSoFar;
        pushVMStack(type, pos);
        return pos;
    }

    void pushVMStack(Walrus::Value::Type type, size_t pos, size_t localIndex = std::numeric_limits<size_t>::max())
    {
        if (m_inPreprocess) {
            if (localIndex != std::numeric_limits<size_t>::max()) {
                size_t pushCount = 0;
                for (const auto& stack : m_vmStack) {
                    if (stack.localIndex() == localIndex) {
                        pushCount++;
                    }
                }
                m_localVariableUsage.push_back(LocalVariableUsage(localIndex, *m_readerOffsetPointer, pushCount));
            }
        }

        m_vmStack.push_back(VMStackInfo(*this, type, pos, m_functionStackSizeSoFar, localIndex));
        m_functionStackSizeSoFar += Walrus::valueStackAllocatedSize(type);
        if (UNLIKELY(m_functionStackSizeSoFar > std::numeric_limits<Walrus::ByteCodeStackOffset>::max())) {
            throw std::string("too many stack usage. we could not support this(yet).");
        }
        m_currentFunction->m_requiredStackSize = std::max(
            m_currentFunction->m_requiredStackSize, m_functionStackSizeSoFar);
    }

    VMStackInfo popVMStackInfo()
    {
        auto info = m_vmStack.back();
        m_functionStackSizeSoFar -= Walrus::valueStackAllocatedSize(info.valueType());
        m_vmStack.pop_back();

        if (m_inPreprocess) {
            if (info.hasValidLocalIndex()) {
                size_t pos = *m_readerOffsetPointer;
                auto iter = m_localVariableUsage.rbegin();
                while (true) {
                    ASSERT(iter != m_localVariableUsage.rend());
                    if (iter->m_localIndex == info.localIndex()
                        && iter->m_endPosition == std::numeric_limits<size_t>::max()) {
                        iter->m_endPosition = *m_readerOffsetPointer;
                        break;
                    }
                    iter++;
                }
            }
        }

        return info;
    }

    size_t popVMStack()
    {
        return popVMStackInfo().position();
    }

    size_t peekVMStack()
    {
        return m_vmStack.back().position();
    }

    const VMStackInfo& peekVMStackInfo()
    {
        return m_vmStack.back();
    }

    Walrus::Value::Type peekVMStackValueType()
    {
        return m_vmStack.back().valueType();
    }

    void beginFunction(Walrus::ModuleFunction* mf)
    {
        m_currentFunction = mf;
        m_currentFunctionType = mf->functionType();
        m_localInfo.clear();
        m_localInfo.reserve(m_currentFunctionType->param().size());
        for (size_t i = 0; i < m_currentFunctionType->param().size(); i++) {
            m_localInfo.push_back(LocalInfo(m_currentFunctionType->param()[i]));
        }
        m_initialFunctionStackSize = m_functionStackSizeSoFar = m_currentFunctionType->paramStackSize();
        m_lastByteCodePosition = 0;
        m_currentFunction->m_requiredStackSize = std::max(
            m_currentFunction->m_requiredStackSize, m_functionStackSizeSoFar);
    }

    void endFunction()
    {
        m_currentFunction = nullptr;
        m_currentFunctionType = nullptr;
        m_vmStack.clear();
        m_shouldContinueToGenerateByteCode = true;
    }

    template <typename CodeType>
    void pushByteCode(const CodeType& code, WASMOpcode opcode)
    {
        m_lastByteCodePosition = m_currentFunction->currentByteCodeSize();
        m_currentFunction->pushByteCode(code);
    }

public:
    WASMBinaryReader()
        : m_readerOffsetPointer(nullptr)
        , m_codeStartOffset(0)
        , m_inPreprocess(false)
        , m_currentFunction(nullptr)
        , m_currentFunctionType(nullptr)
        , m_initialFunctionStackSize(0)
        , m_functionStackSizeSoFar(0)
        , m_lastByteCodePosition(0)
        , m_elementTableIndex(0)
        , m_segmentMode(Walrus::SegmentMode::None)
    {
    }

    ~WASMBinaryReader()
    {
        // clear stack first! because vmStack refer localInfo
        m_vmStack.clear();
        m_localInfo.clear();

        m_result.clear();
    }

    // should be allocated on the stack
    static void* operator new(size_t) = delete;
    static void* operator new[](size_t) = delete;

    virtual void BeginModule(uint32_t version) override
    {
        m_result.m_version = version;
    }

    virtual void EndModule() override {}

    virtual void OnTypeCount(Index count) override
    {
        // TODO reserve vector if possible
    }

    virtual void OnFuncType(Index index,
                            Index paramCount,
                            Type* paramTypes,
                            Index resultCount,
                            Type* resultTypes) override
    {
        Walrus::ValueTypeVector* param = new Walrus::ValueTypeVector();
        param->reserve(paramCount);
        for (size_t i = 0; i < paramCount; i++) {
            param->push_back(toValueKind(paramTypes[i]));
        }
        Walrus::ValueTypeVector* result = new Walrus::ValueTypeVector();
        for (size_t i = 0; i < resultCount; i++) {
            result->push_back(toValueKind(resultTypes[i]));
        }
        ASSERT(index == m_result.m_functionTypes.size());
        m_result.m_functionTypes.push_back(new Walrus::FunctionType(param, result));
    }

    virtual void OnImportCount(Index count) override
    {
        m_result.m_imports.reserve(count);
    }

    virtual void OnImportFunc(Index importIndex,
                              std::string moduleName,
                              std::string fieldName,
                              Index funcIndex,
                              Index sigIndex) override
    {
        ASSERT(m_result.m_functions.size() == funcIndex);
        ASSERT(m_result.m_imports.size() == importIndex);
        Walrus::FunctionType* ft = m_result.m_functionTypes[sigIndex];
        m_result.m_functions.push_back(
            new Walrus::ModuleFunction(ft));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Function,
            moduleName, fieldName, ft));
    }

    virtual void OnImportGlobal(Index importIndex, std::string moduleName, std::string fieldName, Index globalIndex, Type type, bool mutable_) override
    {
        ASSERT(globalIndex == m_result.m_globalTypes.size());
        ASSERT(m_result.m_imports.size() == importIndex);
        m_result.m_globalTypes.push_back(new Walrus::GlobalType(toValueKind(type), mutable_));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Global,
            moduleName, fieldName, m_result.m_globalTypes[globalIndex]));
    }

    virtual void OnImportTable(Index importIndex, std::string moduleName, std::string fieldName, Index tableIndex, Type type, size_t initialSize, size_t maximumSize) override
    {
        ASSERT(tableIndex == m_result.m_tableTypes.size());
        ASSERT(m_result.m_imports.size() == importIndex);
        ASSERT(type == Type::FuncRef || type == Type::ExternRef);

        m_result.m_tableTypes.push_back(new Walrus::TableType(type == Type::FuncRef ? Walrus::Value::Type::FuncRef : Walrus::Value::Type::ExternRef, initialSize, maximumSize));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Table,
            moduleName, fieldName, m_result.m_tableTypes[tableIndex]));
    }

    virtual void OnImportMemory(Index importIndex, std::string moduleName, std::string fieldName, Index memoryIndex, size_t initialSize, size_t maximumSize) override
    {
        ASSERT(memoryIndex == m_result.m_memoryTypes.size());
        ASSERT(m_result.m_imports.size() == importIndex);
        m_result.m_memoryTypes.push_back(new Walrus::MemoryType(initialSize, maximumSize));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Memory,
            moduleName, fieldName, m_result.m_memoryTypes[memoryIndex]));
    }

    virtual void OnImportTag(Index importIndex, std::string moduleName, std::string fieldName, Index tagIndex, Index sigIndex) override
    {
        ASSERT(tagIndex == m_result.m_tagTypes.size());
        ASSERT(m_result.m_imports.size() == importIndex);
        m_result.m_tagTypes.push_back(new Walrus::TagType(sigIndex));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Tag,
            moduleName, fieldName, m_result.m_tagTypes[tagIndex]));
    }

    virtual void OnExportCount(Index count) override
    {
        m_result.m_exports.reserve(count);
    }

    virtual void OnExport(int kind, Index exportIndex, std::string name, Index itemIndex) override
    {
        ASSERT(m_result.m_exports.size() == exportIndex);
        m_result.m_exports.push_back(new Walrus::ExportType(static_cast<Walrus::ExportType::Type>(kind), name, itemIndex));
    }

    /* Table section */
    virtual void OnTableCount(Index count) override
    {
        m_result.m_tableTypes.reserve(count);
    }

    virtual void OnTable(Index index, Type type, size_t initialSize, size_t maximumSize) override
    {
        ASSERT(index == m_result.m_tableTypes.size());
        ASSERT(type == Type::FuncRef || type == Type::ExternRef);
        m_result.m_tableTypes.push_back(new Walrus::TableType(type == Type::FuncRef ? Walrus::Value::Type::FuncRef : Walrus::Value::Type::ExternRef, initialSize, maximumSize));
    }

    virtual void OnElemSegmentCount(Index count) override
    {
        m_result.m_elements.reserve(count);
    }

    virtual void BeginElemSegment(Index index, Index tableIndex, uint8_t flags) override
    {
        m_elementTableIndex = tableIndex;
        m_elementModuleFunction = nullptr;
        m_segmentMode = toSegmentMode(flags);
    }

    virtual void BeginElemSegmentInitExpr(Index index) override
    {
        beginFunction(new Walrus::ModuleFunction(Walrus::Store::getDefaultFunctionType(Walrus::Value::I32)));
    }

    virtual void EndElemSegmentInitExpr(Index index) override
    {
        m_elementModuleFunction = m_currentFunction;
        endFunction();
    }

    virtual void OnElemSegmentElemType(Index index, Type elemType) override
    {
    }

    virtual void OnElemSegmentElemExprCount(Index index, Index count) override
    {
        m_elementFunctionIndex.reserve(count);
    }

    virtual void OnElemSegmentElemExpr_RefNull(Index segmentIndex, Type type) override
    {
        m_elementFunctionIndex.push_back(std::numeric_limits<uint32_t>::max());
    }

    virtual void OnElemSegmentElemExpr_RefFunc(Index segmentIndex, Index funcIndex) override
    {
        m_elementFunctionIndex.push_back(funcIndex);
    }

    virtual void EndElemSegment(Index index) override
    {
        ASSERT(m_result.m_elements.size() == index);
        if (m_elementModuleFunction) {
            m_result.m_elements.push_back(new Walrus::Element(m_segmentMode, m_elementTableIndex, m_elementModuleFunction.value(), std::move(m_elementFunctionIndex)));
        } else {
            m_result.m_elements.push_back(new Walrus::Element(m_segmentMode, m_elementTableIndex, std::move(m_elementFunctionIndex)));
        }

        m_elementModuleFunction = nullptr;
        m_elementTableIndex = 0;
        m_elementFunctionIndex.clear();
        m_segmentMode = Walrus::SegmentMode::None;
    }

    /* Memory section */
    virtual void OnMemoryCount(Index count) override
    {
        m_result.m_memoryTypes.reserve(count);
    }

    virtual void OnMemory(Index index, uint64_t initialSize, uint64_t maximumSize) override
    {
        ASSERT(index == m_result.m_memoryTypes.size());
        m_result.m_memoryTypes.push_back(new Walrus::MemoryType(initialSize, maximumSize));
    }

    virtual void OnDataSegmentCount(Index count) override
    {
        m_result.m_datas.reserve(count);
    }

    virtual void BeginDataSegment(Index index, Index memoryIndex, uint8_t flags) override
    {
        ASSERT(index == m_result.m_datas.size());
        beginFunction(new Walrus::ModuleFunction(Walrus::Store::getDefaultFunctionType(Walrus::Value::I32)));
    }

    virtual void BeginDataSegmentInitExpr(Index index) override
    {
    }

    virtual void EndDataSegmentInitExpr(Index index) override
    {
    }

    virtual void OnDataSegmentData(Index index, const void* data, Address size) override
    {
        m_memoryInitData.resizeWithUninitializedValues(size);
        memcpy(m_memoryInitData.data(), data, size);
    }

    virtual void EndDataSegment(Index index) override
    {
        ASSERT(index == m_result.m_datas.size());
        m_result.m_datas.push_back(new Walrus::Data(m_currentFunction, std::move(m_memoryInitData)));
        endFunction();
    }

    /* Function section */
    virtual void OnFunctionCount(Index count) override
    {
        m_result.m_functions.reserve(count);
    }

    virtual void OnFunction(Index index, Index sigIndex) override
    {
        ASSERT(m_currentFunction == nullptr);
        ASSERT(m_currentFunctionType == nullptr);
        ASSERT(m_result.m_functions.size() == index);
        m_result.m_functions.push_back(new Walrus::ModuleFunction(m_result.m_functionTypes[sigIndex]));
    }

    virtual void OnGlobalCount(Index count) override
    {
        m_result.m_globalTypes.reserve(count);
    }

    virtual void BeginGlobal(Index index, Type type, bool mutable_) override
    {
        ASSERT(m_result.m_globalTypes.size() == index);
        m_result.m_globalTypes.push_back(new Walrus::GlobalType(toValueKind(type), mutable_));
    }

    virtual void BeginGlobalInitExpr(Index index) override
    {
        auto ft = Walrus::Store::getDefaultFunctionType(m_result.m_globalTypes[index]->type());
        Walrus::ModuleFunction* mf = new Walrus::ModuleFunction(ft);
        m_result.m_globalTypes[index]->setFunction(mf);
        beginFunction(mf);
    }

    virtual void EndGlobalInitExpr(Index index) override
    {
        endFunction();
    }

    virtual void EndGlobal(Index index) override
    {
    }

    virtual void EndGlobalSection() override
    {
    }

    virtual void OnTagCount(Index count) override
    {
        m_result.m_tagTypes.reserve(count);
    }

    virtual void OnTagType(Index index, Index sigIndex) override
    {
        ASSERT(index == m_result.m_tagTypes.size());
        m_result.m_tagTypes.push_back(new Walrus::TagType(sigIndex));
    }

    virtual void OnStartFunction(Index funcIndex) override
    {
        m_result.m_seenStartAttribute = true;
        m_result.m_start = funcIndex;
    }

    virtual void BeginFunctionBody(Index index, Offset size) override
    {
        ASSERT(m_currentFunction == nullptr);
        beginFunction(m_result.m_functions[index]);
    }

    virtual void OnLocalDeclCount(Index count) override
    {
        m_currentFunction->m_local.reserve(count);
        m_localInfo.reserve(count + m_currentFunctionType->param().size());
    }

    virtual void OnLocalDecl(Index decl_index, Index count, Type type) override
    {
        while (count) {
            auto wType = toValueKind(type);
            m_currentFunction->m_local.push_back(wType);
            m_localInfo.push_back(LocalInfo(wType));
            auto sz = Walrus::valueStackAllocatedSize(wType);
            m_initialFunctionStackSize += sz;
            m_functionStackSizeSoFar += sz;
            m_currentFunction->m_requiredStackSizeDueToLocal += sz;
            count--;
        }
        m_currentFunction->m_requiredStackSize = std::max(
            m_currentFunction->m_requiredStackSize, m_functionStackSizeSoFar);
    }

    virtual void OnStartReadInstructions() override
    {
        m_codeStartOffset = *m_readerOffsetPointer;
    }

    virtual void OnStartPreprocess() override
    {
        m_inPreprocess = true;
        m_localVariableUsage.clear();
    }

    virtual void OnEndPreprocess() override
    {
        m_inPreprocess = false;
        m_skipValidationUntil = *m_readerOffsetPointer - 1;
        m_shouldContinueToGenerateByteCode = true;

        m_currentFunction->m_byteCode.clear();
        m_currentFunction->m_catchInfo.clear();
        m_blockInfo.clear();
        m_catchInfo.clear();

        m_functionStackSizeSoFar = m_initialFunctionStackSize;
        m_lastByteCodePosition = 0;
        m_vmStack.clear();
    }

    virtual void OnOpcode(uint32_t opcode) override
    {
    }

    virtual void OnCallExpr(uint32_t index) override
    {
        auto functionType = m_result.m_functions[index]->functionType();
        auto callPos = m_currentFunction->currentByteCodeSize();
        pushByteCode(Walrus::Call(index, functionType->param().size() + functionType->result().size()
#if !defined(NDEBUG)
                                             ,
                                  functionType
#endif
                                  ),
                     WASMOpcode::CallOpcode);

        m_currentFunction->expandByteCode(sizeof(Walrus::ByteCodeStackOffset) * (functionType->param().size() + functionType->result().size()));
        auto code = m_currentFunction->peekByteCode<Walrus::Call>(callPos);

        size_t c = 0;
        size_t siz = functionType->param().size();
        for (size_t i = 0; i < siz; i++) {
            ASSERT(peekVMStackValueType() == functionType->param()[siz - i - 1]);
            code->stackOffsets()[siz - c++ - 1] = popVMStack();
        }
        siz = functionType->result().size();
        for (size_t i = 0; i < siz; i++) {
            code->stackOffsets()[c++] = pushVMStack(functionType->result()[i]);
        }
    }

    virtual void OnCallIndirectExpr(Index sigIndex, Index tableIndex) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::I32);
        auto functionType = m_result.m_functionTypes[sigIndex];
        auto callPos = m_currentFunction->currentByteCodeSize();
        pushByteCode(Walrus::CallIndirect(popVMStack(), tableIndex, functionType), WASMOpcode::CallIndirectOpcode);
        m_currentFunction->expandByteCode(sizeof(Walrus::ByteCodeStackOffset) * (functionType->param().size() + functionType->result().size()));

        auto code = m_currentFunction->peekByteCode<Walrus::CallIndirect>(callPos);

        size_t c = 0;
        size_t siz = functionType->param().size();
        for (size_t i = 0; i < siz; i++) {
            ASSERT(peekVMStackValueType() == functionType->param()[siz - i - 1]);
            code->stackOffsets()[siz - c++ - 1] = popVMStack();
        }
        siz = functionType->result().size();
        for (size_t i = 0; i < siz; i++) {
            code->stackOffsets()[c++] = pushVMStack(functionType->result()[i]);
        }
    }

    virtual void OnI32ConstExpr(uint32_t value) override
    {
        pushByteCode(Walrus::Const32(pushVMStack(Walrus::Value::Type::I32), value), WASMOpcode::I32ConstOpcode);
    }

    virtual void OnI64ConstExpr(uint64_t value) override
    {
        pushByteCode(Walrus::Const64(pushVMStack(Walrus::Value::Type::I64), value), WASMOpcode::I64ConstOpcode);
    }

    virtual void OnF32ConstExpr(uint32_t value) override
    {
        pushByteCode(Walrus::Const32(pushVMStack(Walrus::Value::Type::F32), value), WASMOpcode::F32ConstOpcode);
    }

    virtual void OnF64ConstExpr(uint64_t value) override
    {
        pushByteCode(Walrus::Const64(pushVMStack(Walrus::Value::Type::F64), value), WASMOpcode::F64ConstOpcode);
    }

    virtual void OnV128ConstExpr(uint8_t* value) override
    {
        pushByteCode(Walrus::Const128(pushVMStack(Walrus::Value::Type::V128), value), WASMOpcode::V128ConstOpcode);
    }

    std::pair<uint32_t, uint32_t> resolveLocalOffsetAndSize(Index localIndex)
    {
        if (localIndex < m_currentFunctionType->param().size()) {
            size_t offset = 0;
            for (Index i = 0; i < localIndex; i++) {
                offset += Walrus::valueStackAllocatedSize(m_currentFunctionType->param()[i]);
            }
            return std::make_pair(offset, Walrus::valueStackAllocatedSize(m_currentFunctionType->param()[localIndex]));
        } else {
            localIndex -= m_currentFunctionType->param().size();
            size_t offset = m_currentFunctionType->paramStackSize();
            for (Index i = 0; i < localIndex; i++) {
                offset += Walrus::valueStackAllocatedSize(m_currentFunction->m_local[i]);
            }
            return std::make_pair(offset, Walrus::valueStackAllocatedSize(m_currentFunction->m_local[localIndex]));
        }
    }

    Index resolveLocalIndexFromStackPosition(size_t pos)
    {
        ASSERT(pos < m_initialFunctionStackSize);
        if (pos <= m_currentFunctionType->paramStackSize()) {
            Index idx = 0;
            size_t offset = 0;
            while (true) {
                if (offset == pos) {
                    return idx;
                }
                offset += Walrus::valueStackAllocatedSize(m_currentFunctionType->param()[idx]);
                idx++;
            }
        }
        pos -= m_currentFunctionType->paramStackSize();
        Index idx = 0;
        size_t offset = 0;
        while (true) {
            if (offset == pos) {
                return idx + m_currentFunctionType->param().size();
            }
            offset += Walrus::valueStackAllocatedSize(m_currentFunction->m_local[idx]);
            idx++;
        }
    }

    virtual void OnLocalGetExpr(Index localIndex) override
    {
        auto r = resolveLocalOffsetAndSize(localIndex);
        auto localValueType = m_localInfo[localIndex].m_valueType;

        bool canUseDirectReference = true;
        size_t pos = *m_readerOffsetPointer;
        for (const auto& r : m_localVariableUsage) {
            if (r.m_localIndex == localIndex && r.m_startPosition <= pos && pos <= r.m_endPosition) {
                if (r.m_hasWriteUsage) {
                    canUseDirectReference = false;
                    break;
                }
            }
        }

        if (canUseDirectReference) {
            pushVMStack(localValueType, r.first, localIndex);
        } else {
            auto pos = m_functionStackSizeSoFar;
            pushVMStack(localValueType, pos, localIndex);
            generateMoveCodeIfNeeds(r.first, pos, localValueType);
        }
    }

    void updateWriteUsageOfLocalIfNeeds(Index localIndex)
    {
        if (m_inPreprocess) {
            size_t pos = *m_readerOffsetPointer;
            auto iter = m_localVariableUsage.begin();
            while (iter != m_localVariableUsage.end()) {
                if (localIndex == iter->m_localIndex
                    && iter->m_startPosition <= pos && pos <= iter->m_endPosition) {
                    iter->m_hasWriteUsage = true;
                }
                iter++;
            }
        }
    }

    virtual void OnLocalSetExpr(Index localIndex) override
    {
        auto r = resolveLocalOffsetAndSize(localIndex);

        ASSERT(m_localInfo[localIndex].m_valueType == peekVMStackValueType());
        auto src = popVMStackInfo();
        generateMoveCodeIfNeeds(src.position(), r.first, src.valueType());
        updateWriteUsageOfLocalIfNeeds(localIndex);
    }

    virtual void OnLocalTeeExpr(Index localIndex) override
    {
        auto valueType = m_localInfo[localIndex].m_valueType;
        auto r = resolveLocalOffsetAndSize(localIndex);
        ASSERT(valueType == peekVMStackValueType());
        auto dstInfo = peekVMStackInfo();
        generateMoveCodeIfNeeds(dstInfo.position(), r.first, valueType);
        updateWriteUsageOfLocalIfNeeds(localIndex);
    }

    virtual void OnGlobalGetExpr(Index index) override
    {
        auto valueType = m_result.m_globalTypes[index]->type();
        auto sz = Walrus::valueStackAllocatedSize(valueType);
        auto stackPos = pushVMStack(valueType);
        if (sz == 4) {
            pushByteCode(Walrus::GlobalGet32(stackPos, index), WASMOpcode::GlobalGetOpcode);
        } else if (sz == 8) {
            pushByteCode(Walrus::GlobalGet64(stackPos, index), WASMOpcode::GlobalGetOpcode);
        } else {
            ASSERT(sz == 16);
            pushByteCode(Walrus::GlobalGet128(stackPos, index), WASMOpcode::GlobalGetOpcode);
        }
    }

    virtual void OnGlobalSetExpr(Index index) override
    {
        auto valueType = m_result.m_globalTypes[index]->type();
        auto stackPos = peekVMStack();

        ASSERT(peekVMStackValueType() == valueType);
        auto sz = Walrus::valueStackAllocatedSize(valueType);
        if (sz == 4) {
            pushByteCode(Walrus::GlobalSet32(stackPos, index), WASMOpcode::GlobalSetOpcode);
        } else if (sz == 8) {
            pushByteCode(Walrus::GlobalSet64(stackPos, index), WASMOpcode::GlobalSetOpcode);
        } else {
            pushByteCode(Walrus::GlobalSet128(stackPos, index), WASMOpcode::GlobalSetOpcode);
        }
        popVMStack();
    }

    virtual void OnDropExpr() override
    {
        popVMStack();
    }

    virtual void OnBinaryExpr(uint32_t opcode) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType());
        auto src1 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src0 = popVMStack();
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        generateBinaryCode(code, src0, src1, dst);
    }

    virtual void OnUnaryExpr(uint32_t opcode) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src = popVMStack();
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
        case WASMOpcode::I32ReinterpretF32Opcode:
        case WASMOpcode::I64ReinterpretF64Opcode:
        case WASMOpcode::F32ReinterpretI32Opcode:
        case WASMOpcode::F64ReinterpretI64Opcode:
            generateMoveCodeIfNeeds(src, dst, WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
            break;
        default:
            generateUnaryCode(code, src, dst);
            break;
        }
    }

    virtual void OnTernaryExpr(uint32_t opcode) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[2]) == peekVMStackValueType());
        auto c = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType());
        auto rhs = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto lhs = popVMStack();
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
        case WASMOpcode::V128BitSelectOpcode:
            pushByteCode(Walrus::V128BitSelect(lhs, rhs, c, dst), code);
            break;
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    virtual void OnIfExpr(Type sigType) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto stackPos = popVMStack();

        BlockInfo b(BlockInfo::IfElse, sigType, *this);
        b.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJumpIf, b.m_position });
        m_blockInfo.push_back(b);
        pushByteCode(Walrus::JumpIfFalse(stackPos), WASMOpcode::IfOpcode);
    }

    void restoreVMStackBy(const BlockInfo& blockInfo)
    {
        if (blockInfo.m_vmStack.size() <= m_vmStack.size()) {
            size_t diff = m_vmStack.size() - blockInfo.m_vmStack.size();
            for (size_t i = 0; i < diff; i++) {
                popVMStack();
            }
            ASSERT(blockInfo.m_vmStack.size() == m_vmStack.size());
        }
        m_vmStack = blockInfo.m_vmStack;
        m_functionStackSizeSoFar = blockInfo.m_functionStackSizeSoFar;
    }

    void restoreVMStackRegardToPartOfBlockEnd(const BlockInfo& blockInfo)
    {
        if (blockInfo.m_shouldRestoreVMStackAtEnd) {
            restoreVMStackBy(blockInfo);
        } else if (blockInfo.m_returnValueType.IsIndex()) {
            auto ft = m_result.m_functionTypes[blockInfo.m_returnValueType];
            if (ft->param().size()) {
                restoreVMStackBy(blockInfo);
            } else {
                const auto& result = ft->result();
                for (size_t i = 0; i < result.size(); i++) {
                    ASSERT(peekVMStackValueType() == result[result.size() - i - 1]);
                    popVMStack();
                }
            }
        } else if (blockInfo.m_returnValueType != Type::Void) {
            ASSERT(peekVMStackValueType() == toValueKind(blockInfo.m_returnValueType));
            popVMStack();
        }
    }

    void keepSubResultsIfNeeds()
    {
        BlockInfo& blockInfo = m_blockInfo.back();
        if ((blockInfo.m_returnValueType.IsIndex() && m_result.m_functionTypes[blockInfo.m_returnValueType]->result().size())
            || blockInfo.m_returnValueType != Type::Void) {
            blockInfo.m_shouldRestoreVMStackAtEnd = true;
            auto dropSize = dropStackValuesBeforeBrIfNeeds(0);
            if (dropSize.second) {
                generateMoveValuesCodeRegardToDrop(dropSize);
            }
        }
    }

    virtual void OnElseExpr() override
    {
        keepSubResultsIfNeeds();
        BlockInfo& blockInfo = m_blockInfo.back();

        ASSERT(blockInfo.m_blockType == BlockInfo::IfElse);
        blockInfo.m_jumpToEndBrInfo.erase(blockInfo.m_jumpToEndBrInfo.begin());

        if (!blockInfo.m_byteCodeGenerationStopped) {
            blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
            pushByteCode(Walrus::Jump(), WASMOpcode::ElseOpcode);
        }

        blockInfo.m_byteCodeGenerationStopped = false;
        restoreVMStackRegardToPartOfBlockEnd(blockInfo);
        m_currentFunction->peekByteCode<Walrus::JumpIfFalse>(blockInfo.m_position)
            ->setOffset(m_currentFunction->currentByteCodeSize() - blockInfo.m_position);
    }

    virtual void OnLoopExpr(Type sigType) override
    {
        BlockInfo b(BlockInfo::Loop, sigType, *this);
        m_blockInfo.push_back(b);
    }

    virtual void OnBlockExpr(Type sigType) override
    {
        BlockInfo b(BlockInfo::Block, sigType, *this);
        m_blockInfo.push_back(b);
    }

    BlockInfo& findBlockInfoInBr(Index depth)
    {
        ASSERT(m_blockInfo.size());
        auto iter = m_blockInfo.rbegin();
        while (depth) {
            iter++;
            depth--;
        }
        return *iter;
    }

    void stopToGenerateByteCodeWhileBlockEnd()
    {
        if (m_resumeGenerateByteCodeAfterNBlockEnd) {
            return;
        }

        if (m_blockInfo.size()) {
            m_resumeGenerateByteCodeAfterNBlockEnd = 1;
            auto& blockInfo = m_blockInfo.back();
            blockInfo.m_shouldRestoreVMStackAtEnd = true;
            blockInfo.m_byteCodeGenerationStopped = true;
        } else {
            while (m_vmStack.size()) {
                popVMStack();
            }
        }
        m_shouldContinueToGenerateByteCode = false;
    }

    // return drop size, parameter size
    std::pair<size_t, size_t> dropStackValuesBeforeBrIfNeeds(Index depth)
    {
        size_t dropValueSize = 0;
        size_t parameterSize = 0;
        if (depth < m_blockInfo.size()) {
            auto iter = m_blockInfo.rbegin() + depth;
            if (iter->m_vmStack.size() < m_vmStack.size()) {
                size_t start = iter->m_vmStack.size();
                for (size_t i = start; i < m_vmStack.size(); i++) {
                    dropValueSize += m_vmStack[i].stackAllocatedSize();
                }

                if (iter->m_blockType == BlockInfo::Loop) {
                    if (iter->m_returnValueType.IsIndex()) {
                        auto ft = m_result.m_functionTypes[iter->m_returnValueType];
                        dropValueSize += ft->paramStackSize();
                        parameterSize += ft->paramStackSize();
                    }
                } else {
                    if (iter->m_returnValueType.IsIndex()) {
                        auto ft = m_result.m_functionTypes[iter->m_returnValueType];
                        const auto& result = ft->result();
                        for (size_t i = 0; i < result.size(); i++) {
                            parameterSize += Walrus::valueStackAllocatedSize(result[i]);
                        }
                    } else if (iter->m_returnValueType != Type::Void) {
                        parameterSize += Walrus::valueStackAllocatedSize(toValueKind(iter->m_returnValueType));
                    }
                }
            }
        } else if (m_blockInfo.size()) {
            auto iter = m_blockInfo.begin();
            size_t start = iter->m_vmStack.size();
            for (size_t i = start; i < m_vmStack.size(); i++) {
                dropValueSize += m_vmStack[i].stackAllocatedSize();
            }
        }

        return std::make_pair(dropValueSize, parameterSize);
    }

    void generateMoveCodeIfNeeds(size_t srcPosition, size_t dstPosition, Walrus::Value::Type type)
    {
        size_t size = Walrus::valueSize(type);
        if (srcPosition != dstPosition) {
            if (size == 4) {
                pushByteCode(Walrus::Move32(srcPosition, dstPosition), WASMOpcode::Move32Opcode);
            } else if (size == 8) {
                pushByteCode(Walrus::Move64(srcPosition, dstPosition), WASMOpcode::Move64Opcode);
            } else {
                ASSERT(size == 16);
                pushByteCode(Walrus::Move128(srcPosition, dstPosition), WASMOpcode::Move128Opcode);
            }
        }
    }

    void generateMoveValuesCodeRegardToDrop(std::pair<size_t, size_t> dropSize)
    {
        ASSERT(dropSize.second);
        int64_t remainSize = dropSize.second;
        auto srcIter = m_vmStack.rbegin();
        while (true) {
            remainSize -= srcIter->stackAllocatedSize();
            if (!remainSize) {
                break;
            }
            if (remainSize < 0) {
                // stack mismatch! we don't need to generate code
                return;
            }
            srcIter++;
        }

        remainSize = dropSize.first;
        auto dstIter = m_vmStack.rbegin();
        while (true) {
            remainSize -= dstIter->stackAllocatedSize();
            if (!remainSize) {
                break;
            }
            if (remainSize < 0) {
                // stack mismatch! we don't need to generate code
                return;
            }
            dstIter++;
        }

        // reverse order copy to protect newer values
        remainSize = dropSize.second;
        while (true) {
            generateMoveCodeIfNeeds(srcIter->position(), dstIter->nonOptimizedPosition(), srcIter->valueType());
            remainSize -= srcIter->stackAllocatedSize();
            if (!remainSize) {
                break;
            }
            srcIter--;
            dstIter--;
        }
    }

    void generateEndCode(bool shouldClearVMStack = false)
    {
        if (UNLIKELY(m_currentFunctionType->result().size() > m_vmStack.size())) {
            // error case of global init expr
            return;
        }
        auto pos = m_currentFunction->currentByteCodeSize();
        pushByteCode(Walrus::End(m_currentFunctionType->result().size()), WASMOpcode::EndOpcode);

        auto& result = m_currentFunctionType->result();
        m_currentFunction->expandByteCode(sizeof(Walrus::ByteCodeStackOffset) * result.size());
        Walrus::End* end = m_currentFunction->peekByteCode<Walrus::End>(pos);
        for (size_t i = 0; i < result.size(); i++) {
            end->resultOffsets()[result.size() - i - 1] = (m_vmStack.rbegin() + i)->position();
        }

        if (shouldClearVMStack) {
            for (size_t i = 0; i < result.size(); i++) {
                popVMStack();
            }
        }
    }

    void generateFunctionReturnCode(bool shouldClearVMStack = false)
    {
        for (size_t i = 0; i < m_currentFunctionType->result().size(); i++) {
            ASSERT((m_vmStack.rbegin() + i)->valueType() == m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]);
        }
        generateEndCode();
        if (shouldClearVMStack) {
            auto dropSize = dropStackValuesBeforeBrIfNeeds(m_blockInfo.size()).first;
            while (dropSize) {
                dropSize -= popVMStackInfo().stackAllocatedSize();
            }
        } else {
            for (size_t i = 0; i < m_currentFunctionType->result().size(); i++) {
                popVMStack();
            }
            stopToGenerateByteCodeWhileBlockEnd();
        }

        if (!m_blockInfo.size()) {
            // stop to generate bytecode from here!
            m_shouldContinueToGenerateByteCode = false;
            m_resumeGenerateByteCodeAfterNBlockEnd = 0;
        }
    }

    virtual void OnBrExpr(Index depth) override
    {
        if (m_blockInfo.size() == depth) {
            // this case acts like return
            generateFunctionReturnCode(true);
            return;
        }
        auto& blockInfo = findBlockInfoInBr(depth);
        auto offset = (int32_t)blockInfo.m_position - (int32_t)m_currentFunction->currentByteCodeSize();
        auto dropSize = dropStackValuesBeforeBrIfNeeds(depth);
        if (dropSize.second) {
            generateMoveValuesCodeRegardToDrop(dropSize);
        }
        if (blockInfo.m_blockType != BlockInfo::Loop) {
            ASSERT(blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse || blockInfo.m_blockType == BlockInfo::TryCatch);
            blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
        }
        pushByteCode(Walrus::Jump(offset), WASMOpcode::BrOpcode);

        stopToGenerateByteCodeWhileBlockEnd();
    }

    virtual void OnBrIfExpr(Index depth) override
    {
        if (m_blockInfo.size() == depth) {
            // this case acts like return
            ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
            auto stackPos = popVMStack();
            size_t pos = m_currentFunction->currentByteCodeSize();
            pushByteCode(Walrus::JumpIfFalse(stackPos, sizeof(Walrus::JumpIfFalse) + sizeof(Walrus::End) + sizeof(uint16_t) * m_currentFunctionType->result().size()), WASMOpcode::BrIfOpcode);
            for (size_t i = 0; i < m_currentFunctionType->result().size(); i++) {
                ASSERT((m_vmStack.rbegin() + i)->valueType() == m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]);
            }
            generateEndCode();
            return;
        }

        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto stackPos = popVMStack();

        auto& blockInfo = findBlockInfoInBr(depth);
        auto dropSize = dropStackValuesBeforeBrIfNeeds(depth);
        if (dropSize.second) {
            size_t pos = m_currentFunction->currentByteCodeSize();
            pushByteCode(Walrus::JumpIfFalse(stackPos), WASMOpcode::BrIfOpcode);
            generateMoveValuesCodeRegardToDrop(dropSize);
            auto offset = (int32_t)blockInfo.m_position - (int32_t)m_currentFunction->currentByteCodeSize();
            if (blockInfo.m_blockType != BlockInfo::Loop) {
                ASSERT(blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse || blockInfo.m_blockType == BlockInfo::TryCatch);
                blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
            }
            pushByteCode(Walrus::Jump(offset), WASMOpcode::BrIfOpcode);
            m_currentFunction->peekByteCode<Walrus::JumpIfFalse>(pos)
                ->setOffset(m_currentFunction->currentByteCodeSize() - pos);
        } else {
            auto offset = (int32_t)blockInfo.m_position - (int32_t)m_currentFunction->currentByteCodeSize();
            if (blockInfo.m_blockType != BlockInfo::Loop) {
                ASSERT(blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse || blockInfo.m_blockType == BlockInfo::TryCatch);
                blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJumpIf, m_currentFunction->currentByteCodeSize() });
            }
            pushByteCode(Walrus::JumpIfTrue(stackPos, offset), WASMOpcode::BrIfOpcode);
        }
    }

    void emitBrTableCase(size_t brTableCode, Index depth, size_t jumpOffset)
    {
        int32_t offset = (int32_t)(m_currentFunction->currentByteCodeSize() - brTableCode);

        if (m_blockInfo.size() == depth) {
            // this case acts like return
#if !defined(NDEBUG)
            for (size_t i = 0; i < m_currentFunctionType->result().size(); i++) {
                ASSERT((m_vmStack.rbegin() + i)->valueType() == m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]);
            }
#endif
            *(int32_t*)(m_currentFunction->peekByteCode<uint8_t>(brTableCode) + jumpOffset) = offset;
            generateEndCode();
            return;
        }

        auto dropSize = dropStackValuesBeforeBrIfNeeds(depth);

        if (UNLIKELY(dropSize.second)) {
            *(int32_t*)(m_currentFunction->peekByteCode<uint8_t>(brTableCode) + jumpOffset) = offset;
            OnBrExpr(depth);
            return;
        }

        auto& blockInfo = findBlockInfoInBr(depth);

        offset = (int32_t)(blockInfo.m_position - brTableCode);

        if (blockInfo.m_blockType != BlockInfo::Loop) {
            ASSERT(blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse || blockInfo.m_blockType == BlockInfo::TryCatch);
            offset = jumpOffset;
            blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsBrTable, brTableCode + jumpOffset });
        }

        *(int32_t*)(m_currentFunction->peekByteCode<uint8_t>(brTableCode) + jumpOffset) = offset;
    }

    virtual void OnBrTableExpr(Index numTargets, Index* targetDepths, Index defaultTargetDepth) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::I32);
        auto stackPos = popVMStack();

        size_t brTableCode = m_currentFunction->currentByteCodeSize();
        pushByteCode(Walrus::BrTable(stackPos, numTargets), WASMOpcode::BrTableOpcode);

        if (numTargets) {
            m_currentFunction->expandByteCode(sizeof(int32_t) * numTargets);

            for (Index i = 0; i < numTargets; i++) {
                emitBrTableCase(brTableCode, targetDepths[i], sizeof(Walrus::BrTable) + i * sizeof(int32_t));
            }
        }

        // generate default
        emitBrTableCase(brTableCode, defaultTargetDepth, Walrus::BrTable::offsetOfDefault());
        stopToGenerateByteCodeWhileBlockEnd();
    }

    virtual void OnSelectExpr(Index resultCount, Type* resultTypes) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        ASSERT(resultCount == 0 || resultCount == 1);
        auto stackPos = popVMStack();

        auto type = peekVMStackValueType();
        auto src1 = popVMStack();
        auto src0 = popVMStack();
        auto dst = pushVMStack(type);
        pushByteCode(Walrus::Select(stackPos, Walrus::valueSize(type), src0, src1, dst), WASMOpcode::SelectOpcode);
    }

    virtual void OnThrowExpr(Index tagIndex) override
    {
        auto pos = m_currentFunction->currentByteCodeSize();
        uint32_t offsetsSize = 0;

        if (tagIndex != std::numeric_limits<Index>::max()) {
            offsetsSize = m_result.m_functionTypes[m_result.m_tagTypes[tagIndex]->sigIndex()]->param().size();
        }

        pushByteCode(Walrus::Throw(tagIndex, offsetsSize), WASMOpcode::ThrowOpcode);

        if (tagIndex != std::numeric_limits<Index>::max()) {
            auto functionType = m_result.m_functionTypes[m_result.m_tagTypes[tagIndex]->sigIndex()];
            auto& param = functionType->param();
            m_currentFunction->expandByteCode(sizeof(uint16_t) * param.size());
            Walrus::Throw* code = m_currentFunction->peekByteCode<Walrus::Throw>(pos);
            for (size_t i = 0; i < param.size(); i++) {
                code->dataOffsets()[param.size() - i - 1] = (m_vmStack.rbegin() + i)->position();
            }
            for (size_t i = 0; i < param.size(); i++) {
                ASSERT(peekVMStackValueType() == functionType->param()[functionType->param().size() - i - 1]);
                popVMStack();
            }
        }

        stopToGenerateByteCodeWhileBlockEnd();
    }

    virtual void OnTryExpr(Type sigType) override
    {
        BlockInfo b(BlockInfo::TryCatch, sigType, *this);
        m_blockInfo.push_back(b);
    }

    void processCatchExpr(Index tagIndex)
    {
        ASSERT(m_blockInfo.back().m_blockType == BlockInfo::TryCatch);
        keepSubResultsIfNeeds();

        auto& blockInfo = m_blockInfo.back();
        restoreVMStackRegardToPartOfBlockEnd(blockInfo);

        size_t tryEnd = m_currentFunction->currentByteCodeSize();
        if (m_catchInfo.size() && m_catchInfo.back().m_tryCatchBlockDepth == m_blockInfo.size()) {
            // not first catch
            tryEnd = m_catchInfo.back().m_tryEnd;
        }

        if (!blockInfo.m_byteCodeGenerationStopped) {
            blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
            pushByteCode(Walrus::Jump(), WASMOpcode::CatchOpcode);
        }

        blockInfo.m_byteCodeGenerationStopped = false;

        m_catchInfo.push_back({ m_blockInfo.size(), m_blockInfo.back().m_position, tryEnd, m_currentFunction->currentByteCodeSize(), tagIndex });

        if (tagIndex != std::numeric_limits<Index>::max()) {
            auto functionType = m_result.m_functionTypes[m_result.m_tagTypes[tagIndex]->sigIndex()];
            for (size_t i = 0; i < functionType->param().size(); i++) {
                pushVMStack(functionType->param()[i]);
            }
        }
    }

    virtual void OnCatchExpr(Index tagIndex) override
    {
        processCatchExpr(tagIndex);
    }

    virtual void OnCatchAllExpr() override
    {
        processCatchExpr(std::numeric_limits<Index>::max());
    }

    virtual void OnMemoryInitExpr(Index segmentIndex, Index memidx) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src2 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();

        pushByteCode(Walrus::MemoryInit(memidx, segmentIndex, src0, src1, src2), WASMOpcode::MemoryInitOpcode);
    }

    virtual void OnMemoryCopyExpr(Index srcMemIndex, Index dstMemIndex) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src2 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();

        pushByteCode(Walrus::MemoryCopy(srcMemIndex, dstMemIndex, src0, src1, src2), WASMOpcode::MemoryCopyOpcode);
    }

    virtual void OnMemoryFillExpr(Index memidx) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src2 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();

        pushByteCode(Walrus::MemoryFill(memidx, src0, src1, src2), WASMOpcode::MemoryFillOpcode);
    }

    virtual void OnDataDropExpr(Index segmentIndex) override
    {
        pushByteCode(Walrus::DataDrop(segmentIndex), WASMOpcode::DataDropOpcode);
    }

    virtual void OnMemoryGrowExpr(Index memidx) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src = popVMStack();
        auto dst = pushVMStack(Walrus::Value::Type::I32);
        pushByteCode(Walrus::MemoryGrow(memidx, src, dst), WASMOpcode::MemoryGrowOpcode);
    }

    virtual void OnMemorySizeExpr(Index memidx) override
    {
        auto stackPos = pushVMStack(Walrus::Value::Type::I32);
        pushByteCode(Walrus::MemorySize(memidx, stackPos), WASMOpcode::MemorySizeOpcode);
    }

    virtual void OnTableGetExpr(Index tableIndex) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src = popVMStack();
        auto dst = pushVMStack(m_result.m_tableTypes[tableIndex]->type());
        pushByteCode(Walrus::TableGet(tableIndex, src, dst), WASMOpcode::TableGetOpcode);
    }

    virtual void OnTableSetExpr(Index tableIndex) override
    {
        ASSERT(peekVMStackValueType() == m_result.m_tableTypes[tableIndex]->type());
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();
        pushByteCode(Walrus::TableSet(tableIndex, src0, src1), WASMOpcode::TableSetOpcode);
    }

    virtual void OnTableGrowExpr(Index tableIndex) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == m_result.m_tableTypes[tableIndex]->type());
        auto src0 = popVMStack();
        auto dst = pushVMStack(Walrus::Value::Type::I32);
        pushByteCode(Walrus::TableGrow(tableIndex, src0, src1, dst), WASMOpcode::TableGrowOpcode);
    }

    virtual void OnTableSizeExpr(Index tableIndex) override
    {
        auto dst = pushVMStack(Walrus::Value::Type::I32);
        pushByteCode(Walrus::TableSize(tableIndex, dst), WASMOpcode::TableSizeOpcode);
    }

    virtual void OnTableCopyExpr(Index dst_index, Index src_index) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src2 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();
        pushByteCode(Walrus::TableCopy(dst_index, src_index, src0, src1, src2), WASMOpcode::TableCopyOpcode);
    }

    virtual void OnTableFillExpr(Index tableIndex) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src2 = popVMStack();
        ASSERT(peekVMStackValueType() == m_result.m_tableTypes[tableIndex]->type());
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();
        pushByteCode(Walrus::TableFill(tableIndex, src0, src1, src2), WASMOpcode::TableFillOpcode);
    }

    virtual void OnElemDropExpr(Index segmentIndex) override
    {
        pushByteCode(Walrus::ElemDrop(segmentIndex), WASMOpcode::ElemDropOpcode);
    }

    virtual void OnTableInitExpr(Index segmentIndex, Index tableIndex) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src2 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();
        pushByteCode(Walrus::TableInit(tableIndex, segmentIndex, src0, src1, src2), WASMOpcode::TableInitOpcode);
    }

    virtual void OnLoadExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src = popVMStack();
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        if ((opcode == (int)WASMOpcode::I32LoadOpcode || opcode == (int)WASMOpcode::F32LoadOpcode) && offset == 0) {
            pushByteCode(Walrus::Load32(src, dst), code);
        } else if ((opcode == (int)WASMOpcode::I64LoadOpcode || opcode == (int)WASMOpcode::F64LoadOpcode) && offset == 0) {
            pushByteCode(Walrus::Load64(src, dst), code);
        } else {
            generateMemoryLoadCode(code, offset, src, dst);
        }
    }

    virtual void OnStoreExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType());
        auto src1 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src0 = popVMStack();
        if ((opcode == (int)WASMOpcode::I32StoreOpcode || opcode == (int)WASMOpcode::F32StoreOpcode) && offset == 0) {
            pushByteCode(Walrus::Store32(src0, src1), code);
        } else if ((opcode == (int)WASMOpcode::I64StoreOpcode || opcode == (int)WASMOpcode::F64StoreOpcode) && offset == 0) {
            pushByteCode(Walrus::Store64(src0, src1), code);
        } else {
            generateMemoryStoreCode(code, offset, src0, src1);
        }
    }

    virtual void OnRefFuncExpr(Index func_index) override
    {
        auto dst = pushVMStack(Walrus::Value::Type::FuncRef);
        pushByteCode(Walrus::RefFunc(func_index, dst), WASMOpcode::RefFuncOpcode);
    }

    virtual void OnRefNullExpr(Type type) override
    {
        Walrus::ByteCodeStackOffset dst = pushVMStack(toValueKind(type));
#if defined(WALRUS_32)
        pushByteCode(Walrus::Const32(dst, Walrus::Value::Null), WASMOpcode::Const32Opcode);
#else
        pushByteCode(Walrus::Const64(dst, Walrus::Value::Null), WASMOpcode::Const64Opcode);
#endif
    }

    virtual void OnRefIsNullExpr() override
    {
        auto src = popVMStack();
        auto dst = pushVMStack(Walrus::Value::Type::I32);
#if defined(WALRUS_32)
        pushByteCode(Walrus::I32Eqz(src, dst), WASMOpcode::RefIsNullOpcode);
#else
        pushByteCode(Walrus::I64Eqz(src, dst), WASMOpcode::RefIsNullOpcode);
#endif
    }

    virtual void OnNopExpr() override
    {
    }

    virtual void OnReturnExpr() override
    {
        generateFunctionReturnCode();
    }

    virtual void OnEndExpr() override
    {
        if (m_blockInfo.size()) {
            auto dropSize = dropStackValuesBeforeBrIfNeeds(0);
            auto blockInfo = m_blockInfo.back();
            m_blockInfo.pop_back();

#if !defined(NDEBUG)
            if (!blockInfo.m_shouldRestoreVMStackAtEnd) {
                if (!blockInfo.m_returnValueType.IsIndex() && blockInfo.m_returnValueType != Type::Void) {
                    ASSERT(peekVMStackValueType() == toValueKind(blockInfo.m_returnValueType));
                }
            }
#endif

            if (blockInfo.m_blockType == BlockInfo::TryCatch) {
                auto iter = m_catchInfo.begin();
                while (iter != m_catchInfo.end()) {
                    if (iter->m_tryCatchBlockDepth - 1 != m_blockInfo.size()) {
                        iter++;
                        continue;
                    }
                    size_t stackSizeToBe = m_initialFunctionStackSize;
                    for (size_t i = 0; i < blockInfo.m_vmStack.size(); i++) {
                        stackSizeToBe += m_vmStack[i].stackAllocatedSize();
                    }
                    m_currentFunction->m_catchInfo.push_back({ iter->m_tryStart, iter->m_tryEnd, iter->m_catchStart, stackSizeToBe, iter->m_tagIndex });
                    iter = m_catchInfo.erase(iter);
                }
            }

            if (blockInfo.m_byteCodeGenerationStopped && blockInfo.m_jumpToEndBrInfo.size() == 0) {
                stopToGenerateByteCodeWhileBlockEnd();
                return;
            }

            if (blockInfo.m_shouldRestoreVMStackAtEnd) {
                if (dropSize.second) {
                    generateMoveValuesCodeRegardToDrop(dropSize);
                }
                restoreVMStackBy(blockInfo);
                if (blockInfo.m_returnValueType.IsIndex()) {
                    auto ft = m_result.m_functionTypes[blockInfo.m_returnValueType];
                    const auto& param = ft->param();
                    for (size_t i = 0; i < param.size(); i++) {
                        ASSERT(peekVMStackValueType() == param[param.size() - i - 1]);
                        popVMStack();
                    }

                    const auto& result = ft->result();
                    for (size_t i = 0; i < result.size(); i++) {
                        pushVMStack(result[i]);
                    }
                } else if (blockInfo.m_returnValueType != Type::Void) {
                    pushVMStack(toValueKind(blockInfo.m_returnValueType));
                }
            }

            for (size_t i = 0; i < blockInfo.m_jumpToEndBrInfo.size(); i++) {
                switch (blockInfo.m_jumpToEndBrInfo[i].m_type) {
                case BlockInfo::JumpToEndBrInfo::IsJump:
                    m_currentFunction->peekByteCode<Walrus::Jump>(blockInfo.m_jumpToEndBrInfo[i].m_position)->setOffset(m_currentFunction->currentByteCodeSize() - blockInfo.m_jumpToEndBrInfo[i].m_position);
                    break;
                case BlockInfo::JumpToEndBrInfo::IsJumpIf:
                    m_currentFunction->peekByteCode<Walrus::JumpIfFalse>(blockInfo.m_jumpToEndBrInfo[i].m_position)
                        ->setOffset(m_currentFunction->currentByteCodeSize() - blockInfo.m_jumpToEndBrInfo[i].m_position);
                    break;
                default:
                    ASSERT(blockInfo.m_jumpToEndBrInfo[i].m_type == BlockInfo::JumpToEndBrInfo::IsBrTable);

                    int32_t* offset = m_currentFunction->peekByteCode<int32_t>(blockInfo.m_jumpToEndBrInfo[i].m_position);
                    *offset = m_currentFunction->currentByteCodeSize() + (size_t)*offset - blockInfo.m_jumpToEndBrInfo[i].m_position;
                    break;
                }
            }
        } else {
            generateEndCode(true);
        }
    }

    virtual void OnUnreachableExpr() override
    {
        pushByteCode(Walrus::Unreachable(), WASMOpcode::UnreachableOpcode);
        stopToGenerateByteCodeWhileBlockEnd();
    }

    virtual void EndFunctionBody(Index index) override
    {
#if !defined(NDEBUG)
        if (getenv("DUMP_BYTECODE") && strlen(getenv("DUMP_BYTECODE"))) {
            m_currentFunction->dumpByteCode();
        }
        if (m_shouldContinueToGenerateByteCode) {
            for (size_t i = 0; i < m_currentFunctionType->result().size() && m_vmStack.size(); i++) {
                ASSERT(popVMStackInfo().valueType() == m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]);
            }
            ASSERT(m_vmStack.empty());
        }
#endif

        ASSERT(m_currentFunction == m_result.m_functions[index]);
        endFunction();
    }

    // SIMD Instructions
    virtual void OnLoadSplatExpr(int opcode, Index memidx, Address alignment_log2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src = popVMStack();
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
#define GENERATE_LOAD_CODE_CASE(name, ...)                  \
    case WASMOpcode::name##Opcode: {                        \
        pushByteCode(Walrus::name(offset, src, dst), code); \
        break;                                              \
    }

            FOR_EACH_BYTECODE_SIMD_LOAD_SPLAT_OP(GENERATE_LOAD_CODE_CASE)
#undef GENERATE_LOAD_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
        }
    }

    virtual void OnLoadZeroExpr(int opcode, Index memidx, Address alignment_log2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src = popVMStack();
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
        case WASMOpcode::V128Load32ZeroOpcode: {
            pushByteCode(Walrus::V128Load32Zero(offset, src, dst), code);
            break;
        }
        case WASMOpcode::V128Load64ZeroOpcode: {
            pushByteCode(Walrus::V128Load64Zero(offset, src, dst), code);
            break;
        }
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    virtual void OnSimdLaneOpExpr(int opcode, uint64_t value) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        switch (code) {
#define GENERATE_SIMD_EXTRACT_LANE_CODE_CASE(name, ...)                                                              \
    case WASMOpcode::name##Opcode: {                                                                                 \
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType()); \
        auto src = popVMStack();                                                                                     \
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));              \
        pushByteCode(Walrus::name(static_cast<uint8_t>(value), src, dst), code);                                     \
        break;                                                                                                       \
    }

#define GENERATE_SIMD_REPLACE_LANE_CODE_CASE(name, ...)                                                              \
    case WASMOpcode::name##Opcode: {                                                                                 \
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType()); \
        auto src1 = popVMStack();                                                                                    \
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType()); \
        auto src0 = popVMStack();                                                                                    \
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));              \
        pushByteCode(Walrus::name(static_cast<uint8_t>(value), src0, src1, dst), code);                              \
        break;                                                                                                       \
    }

            FOR_EACH_BYTECODE_SIMD_EXTRACT_LANE_OP(GENERATE_SIMD_EXTRACT_LANE_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_REPLACE_LANE_OP(GENERATE_SIMD_REPLACE_LANE_CODE_CASE)
#undef GENERATE_SIMD_EXTRACT_LANE_CODE_CASE
#undef GENERATE_SIMD_REPLACE_LANE_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    virtual void OnSimdLoadLaneExpr(int opcode, Index memidx, Address alignment_log2, Address offset, uint64_t value) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::V128);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
#define GENERATE_LOAD_CODE_CASE(name, opType)                                                                       \
    case WASMOpcode::name##Opcode: {                                                                                \
        pushByteCode(Walrus::name(offset, src0, src1, static_cast<Walrus::ByteCodeStackOffset>(value), dst), code); \
        break;                                                                                                      \
    }
            FOR_EACH_BYTECODE_SIMD_LOAD_LANE_OP(GENERATE_LOAD_CODE_CASE)
#undef GENERATE_LOAD_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
        }
    }

    virtual void OnSimdStoreLaneExpr(int opcode, Index memidx, Address alignment_log2, Address offset, uint64_t value) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::V128);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();
        switch (code) {
#define GENERATE_STORE_CODE_CASE(name, opType)                                                                 \
    case WASMOpcode::name##Opcode: {                                                                           \
        pushByteCode(Walrus::name(offset, src0, src1, static_cast<Walrus::ByteCodeStackOffset>(value)), code); \
        break;                                                                                                 \
    }
            FOR_EACH_BYTECODE_SIMD_STORE_LANE_OP(GENERATE_STORE_CODE_CASE)
#undef GENERATE_STORE_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
        }
    }

    virtual void OnSimdShuffleOpExpr(int opcode, uint8_t* value) override
    {
        ASSERT(static_cast<WASMOpcode>(opcode) == WASMOpcode::I8X16ShuffleOpcode);
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::V128);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::V128);
        auto src0 = popVMStack();
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        pushByteCode(Walrus::I8X16Shuffle(src0, src1, dst, value), WASMOpcode::I8X16ShuffleOpcode);
    }

    void generateBinaryCode(WASMOpcode code, size_t src0, size_t src1, size_t dst)
    {
        switch (code) {
#define GENERATE_BINARY_CODE_CASE(name, ...)               \
    case WASMOpcode::name##Opcode: {                       \
        pushByteCode(Walrus::name(src0, src1, dst), code); \
        break;                                             \
    }
            FOR_EACH_BYTECODE_BINARY_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_SHIFT_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_OTHER(GENERATE_BINARY_CODE_CASE)
#undef GENERATE_BINARY_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    void generateUnaryCode(WASMOpcode code, size_t src, size_t dst)
    {
        switch (code) {
#define GENERATE_UNARY_CODE_CASE(name, ...)         \
    case WASMOpcode::name##Opcode: {                \
        pushByteCode(Walrus::name(src, dst), code); \
        break;                                      \
    }
            FOR_EACH_BYTECODE_UNARY_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_UNARY_OP_2(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_UNARY_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_UNARY_CONVERT_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_UNARY_OTHER(GENERATE_UNARY_CODE_CASE)
#undef GENERATE_UNARY_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    void generateMemoryLoadCode(WASMOpcode code, size_t offset, size_t src, size_t dst)
    {
        switch (code) {
#define GENERATE_LOAD_CODE_CASE(name, readType, writeType)  \
    case WASMOpcode::name##Opcode: {                        \
        pushByteCode(Walrus::name(offset, src, dst), code); \
        break;                                              \
    }
            FOR_EACH_BYTECODE_LOAD_OP(GENERATE_LOAD_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_LOAD_EXTEND_OP(GENERATE_LOAD_CODE_CASE)
#undef GENERATE_LOAD_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    void generateMemoryStoreCode(WASMOpcode code, size_t offset, size_t src0, size_t src1)
    {
        switch (code) {
#define GENERATE_STORE_CODE_CASE(name, readType, writeType)   \
    case WASMOpcode::name##Opcode: {                          \
        pushByteCode(Walrus::name(offset, src0, src1), code); \
        break;                                                \
    }
            FOR_EACH_BYTECODE_STORE_OP(GENERATE_STORE_CODE_CASE)
#undef GENERATE_STORE_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    bool isBinaryOperation(WASMOpcode opcode)
    {
        switch (opcode) {
#define GENERATE_BINARY_CODE_CASE(name, op, paramType, returnType) \
    case WASMOpcode::name##Opcode:
            FOR_EACH_BYTECODE_BINARY_OP(GENERATE_BINARY_CODE_CASE)
#undef GENERATE_BINARY_CODE_CASE
            return true;
        default:
            return false;
        }
    }

    Walrus::WASMParsingResult& parsingResult() { return m_result; }
};

} // namespace wabt

namespace Walrus {

WASMParsingResult::WASMParsingResult()
    : m_seenStartAttribute(false)
    , m_version(0)
    , m_start(0)
{
}

void WASMParsingResult::clear()
{
    for (size_t i = 0; i < m_imports.size(); i++) {
        delete m_imports[i];
    }

    for (size_t i = 0; i < m_exports.size(); i++) {
        delete m_exports[i];
    }

    for (size_t i = 0; i < m_functions.size(); i++) {
        delete m_functions[i];
    }

    for (size_t i = 0; i < m_datas.size(); i++) {
        delete m_datas[i];
    }

    for (size_t i = 0; i < m_elements.size(); i++) {
        delete m_elements[i];
    }

    for (size_t i = 0; i < m_functionTypes.size(); i++) {
        delete m_functionTypes[i];
    }

    for (size_t i = 0; i < m_globalTypes.size(); i++) {
        delete m_globalTypes[i];
    }

    for (size_t i = 0; i < m_tableTypes.size(); i++) {
        delete m_tableTypes[i];
    }

    for (size_t i = 0; i < m_memoryTypes.size(); i++) {
        delete m_memoryTypes[i];
    }

    for (size_t i = 0; i < m_tagTypes.size(); i++) {
        delete m_tagTypes[i];
    }
}

std::pair<Optional<Module*>, std::string> WASMParser::parseBinary(Store* store, const std::string& filename, const uint8_t* data, size_t len)
{
    wabt::WASMBinaryReader delegate;

    std::string error = ReadWasmBinary(filename, data, len, &delegate);
    if (error.length()) {
        return std::make_pair(nullptr, error);
    }

    Module* module = new Module(store, delegate.parsingResult());
    return std::make_pair(module, std::string());
}

} // namespace Walrus
