#include "Ext.h"

#include <evm.h>

#include "preprocessor/llvm_includes_start.h"
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include "preprocessor/llvm_includes_end.h"

#include "RuntimeManager.h"
#include "Memory.h"
#include "Type.h"
#include "Endianness.h"

namespace dev
{
namespace eth
{
namespace jit
{

Ext::Ext(RuntimeManager& _runtimeManager, Memory& _memoryMan) :
	RuntimeHelper(_runtimeManager),
	m_memoryMan(_memoryMan)
{
	m_funcs = decltype(m_funcs)();
	m_argAllocas = decltype(m_argAllocas)();
	m_size = m_builder.CreateAlloca(Type::Size, nullptr, "env.size");
}

namespace
{

using FuncDesc = std::tuple<char const*, llvm::FunctionType*>;

llvm::FunctionType* getFunctionType(llvm::Type* _returnType, std::initializer_list<llvm::Type*> const& _argsTypes)
{
	return llvm::FunctionType::get(_returnType, llvm::ArrayRef<llvm::Type*>{_argsTypes.begin(), _argsTypes.size()}, false);
}

std::array<FuncDesc, sizeOf<EnvFunc>::value> const& getEnvFuncDescs()
{
	static std::array<FuncDesc, sizeOf<EnvFunc>::value> descs{{
		FuncDesc{"env_sload",   getFunctionType(Type::Void, {Type::EnvPtr, Type::WordPtr, Type::WordPtr})},
		FuncDesc{"env_sstore",  getFunctionType(Type::Void, {Type::EnvPtr, Type::WordPtr, Type::WordPtr})},
		FuncDesc{"env_sha3", getFunctionType(Type::Void, {Type::BytePtr, Type::Size, Type::WordPtr})},
		FuncDesc{"env_balance", getFunctionType(Type::Void, {Type::WordPtr, Type::EnvPtr, Type::WordPtr})},
		FuncDesc{"env_create", getFunctionType(Type::Void, {Type::EnvPtr, Type::GasPtr, Type::WordPtr, Type::BytePtr, Type::Size, Type::WordPtr})},
		FuncDesc{"env_call", getFunctionType(Type::Bool, {Type::EnvPtr, Type::GasPtr, Type::Gas, Type::WordPtr, Type::WordPtr, Type::WordPtr, Type::WordPtr, Type::WordPtr, Type::BytePtr, Type::Size, Type::BytePtr, Type::Size})},
		FuncDesc{"env_log", getFunctionType(Type::Void, {Type::EnvPtr, Type::BytePtr, Type::Size, Type::WordPtr, Type::WordPtr, Type::WordPtr, Type::WordPtr})},
		FuncDesc{"env_blockhash", getFunctionType(Type::Void, {Type::EnvPtr, Type::WordPtr, Type::WordPtr})},
		FuncDesc{"env_extcode", getFunctionType(Type::BytePtr, {Type::EnvPtr, Type::WordPtr, Type::Size->getPointerTo()})},
	}};

	return descs;
}

llvm::Function* createFunc(EnvFunc _id, llvm::Module* _module)
{
	auto&& desc = getEnvFuncDescs()[static_cast<size_t>(_id)];
	return llvm::Function::Create(std::get<1>(desc), llvm::Function::ExternalLinkage, std::get<0>(desc), _module);
}

llvm::Function* getQueryFunc(llvm::Module* _module)
{
	static const auto funcName = "evm.query";
	auto func = _module->getFunction(funcName);
	if (!func)
	{
		// TODO: Mark the function as pure to eliminate multiple calls.
		auto i32 = llvm::IntegerType::getInt32Ty(_module->getContext());
		auto fty = llvm::FunctionType::get(Type::Void, {Type::WordPtr, Type::EnvPtr, i32, Type::WordPtr}, false);
		func = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, funcName, _module);
		func->addAttribute(1, llvm::Attribute::StructRet);
		func->addAttribute(1, llvm::Attribute::NoAlias);
		func->addAttribute(1, llvm::Attribute::NoCapture);
		func->addAttribute(4, llvm::Attribute::ByVal);
		func->addAttribute(4, llvm::Attribute::ReadOnly);
		func->addAttribute(4, llvm::Attribute::NoAlias);
		func->addAttribute(4, llvm::Attribute::NoCapture);
	}
	return func;
}

llvm::Function* getUpdateFunc(llvm::Module* _module)
{
	static const auto funcName = "evm.update";
	auto func = _module->getFunction(funcName);
	if (!func)
	{
		auto i32 = llvm::IntegerType::getInt32Ty(_module->getContext());
		auto fty = llvm::FunctionType::get(Type::Void, {Type::EnvPtr, i32, Type::WordPtr, Type::WordPtr}, false);
		func = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, funcName, _module);
		func->addAttribute(3, llvm::Attribute::ByVal);
		func->addAttribute(3, llvm::Attribute::ReadOnly);
		func->addAttribute(3, llvm::Attribute::NoAlias);
		func->addAttribute(3, llvm::Attribute::NoCapture);
		func->addAttribute(4, llvm::Attribute::ByVal);
		func->addAttribute(4, llvm::Attribute::ReadOnly);
		func->addAttribute(4, llvm::Attribute::NoAlias);
		func->addAttribute(4, llvm::Attribute::NoCapture);
	}
	return func;
}

llvm::StructType* getMemRefTy(llvm::Module* _module)
{
	static const auto name = "evm.memref";
	auto ty = _module->getTypeByName(name);
	if (!ty)
		ty = llvm::StructType::create({Type::BytePtr, Type::Size}, name);
	return ty;
}

llvm::Function* getCallFunc(llvm::Module* _module)
{
	static const auto funcName = "evm.call";
	auto func = _module->getFunction(funcName);
	if (!func)
	{
		auto i32 = llvm::IntegerType::getInt32Ty(_module->getContext());
		auto hash160Ty = llvm::IntegerType::getIntNTy(_module->getContext(), 160);
		auto memRefTy = getMemRefTy(_module);
		auto pMemRefTy = memRefTy->getPointerTo();
		// TODO: Should be use Triple here?
		#ifdef _MSC_VER
		// On Windows this argument is passed by pointer.
		auto inputTy = pMemRefTy;
		#else
		// On Unix this argument is passed by value.
		auto inputTy = memRefTy;
		#endif
		auto fty = llvm::FunctionType::get(
			Type::Gas,
			{Type::EnvPtr, i32, Type::Gas, hash160Ty->getPointerTo(), Type::WordPtr, Type::BytePtr, Type::Size, pMemRefTy},
			false);
		func = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, funcName, _module);
		func->addAttribute(4, llvm::Attribute::ByVal);
		func->addAttribute(4, llvm::Attribute::ReadOnly);
		func->addAttribute(4, llvm::Attribute::NoAlias);
		func->addAttribute(4, llvm::Attribute::NoCapture);
		func->addAttribute(5, llvm::Attribute::ByVal);
		func->addAttribute(5, llvm::Attribute::ReadOnly);
		func->addAttribute(5, llvm::Attribute::NoAlias);
		func->addAttribute(5, llvm::Attribute::NoCapture);
		if (inputTy->isPointerTy())
		{
			func->addAttribute(6, llvm::Attribute::ByVal);
			func->addAttribute(6, llvm::Attribute::ReadOnly);
			func->addAttribute(6, llvm::Attribute::NoCapture);
		}
		func->addAttribute(8, llvm::Attribute::ByVal);
		func->addAttribute(8, llvm::Attribute::NoCapture);
	}
	return func;
}

}



llvm::Value* Ext::getArgAlloca()
{
	auto& a = m_argAllocas[m_argCounter];
	if (!a)
	{
		InsertPointGuard g{m_builder};
		auto allocaIt = getMainFunction()->front().begin();
		auto allocaPtr = &(*allocaIt);
		std::advance(allocaIt, m_argCounter); // Skip already created allocas
		m_builder.SetInsertPoint(allocaPtr);
		a = m_builder.CreateAlloca(Type::Word, nullptr, {"a.", std::to_string(m_argCounter)});
	}
	++m_argCounter;
	return a;
}

llvm::Value* Ext::byPtr(llvm::Value* _value)
{
	auto a = getArgAlloca();
	m_builder.CreateStore(_value, a);
	return a;
}

llvm::CallInst* Ext::createCall(EnvFunc _funcId, std::initializer_list<llvm::Value*> const& _args)
{
	auto& func = m_funcs[static_cast<size_t>(_funcId)];
	if (!func)
		func = createFunc(_funcId, getModule());

	m_argCounter = 0;
	return m_builder.CreateCall(func, {_args.begin(), _args.size()});
}

llvm::Value* Ext::createCABICall(llvm::Function* _func, std::initializer_list<llvm::Value*> const& _args, bool _derefOutput)
{
	auto args = llvm::SmallVector<llvm::Value*, 8>{_args};

	auto hasSRet = _func->hasStructRetAttr();
	if (hasSRet)  // Prepare memory for return struct.
		args.insert(args.begin(), getArgAlloca());

	for (auto&& farg: _func->args())
	{
		if (farg.hasByValAttr())
		{
			auto& arg = args[farg.getArgNo()];
			// TODO: Remove defensive check and always use it this way.
			if (!arg->getType()->isPointerTy())
			{
				auto mem = getArgAlloca();
				// TODO: The bitcast may be redundant
				mem = m_builder.CreateBitCast(mem, arg->getType()->getPointerTo());
				m_builder.CreateStore(arg, mem);
				arg = mem;
			}
		}
	}

	m_argCounter = 0;
	llvm::Value* callRet = m_builder.CreateCall(_func, args);
	if (hasSRet)
		return _derefOutput ? m_builder.CreateLoad(args[0]) : args[0];
	else
		return callRet;
}

llvm::Value* Ext::sload(llvm::Value* _index)
{
	auto func = getQueryFunc(getModule());
	return createCABICall(func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_STORAGE), _index});
}

void Ext::sstore(llvm::Value* _index, llvm::Value* _value)
{
	auto func = getUpdateFunc(getModule());
	createCABICall(func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_SSTORE), _index, _value});
}

void Ext::selfdestruct(llvm::Value* _beneficiary)
{
	auto func = getUpdateFunc(getModule());
	auto b = Endianness::toBE(m_builder, _beneficiary);
	auto undef = llvm::UndefValue::get(Type::WordPtr);
	createCABICall(func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_SELFDESTRUCT), b, undef});
}

llvm::Value* Ext::calldataload(llvm::Value* _idx)
{
	auto ret = getArgAlloca();
	auto result = m_builder.CreateBitCast(ret, Type::BytePtr);

	auto callDataSize = getRuntimeManager().getCallDataSize();
	auto callDataSize64 = m_builder.CreateTrunc(callDataSize, Type::Size);
	auto idxValid = m_builder.CreateICmpULT(_idx, callDataSize);
	auto idx = m_builder.CreateTrunc(m_builder.CreateSelect(idxValid, _idx, callDataSize), Type::Size, "idx");

	auto end = m_builder.CreateNUWAdd(idx, m_builder.getInt64(32));
	end = m_builder.CreateSelect(m_builder.CreateICmpULE(end, callDataSize64), end, callDataSize64);
	auto copySize = m_builder.CreateNUWSub(end, idx);
	auto padSize = m_builder.CreateNUWSub(m_builder.getInt64(32), copySize);
	auto dataBegin = m_builder.CreateGEP(Type::Byte, getRuntimeManager().getCallData(), idx);
	m_builder.CreateMemCpy(result, dataBegin, copySize, 1);
	auto pad = m_builder.CreateGEP(Type::Byte, result, copySize);
	m_builder.CreateMemSet(pad, m_builder.getInt8(0), padSize, 1);

	m_argCounter = 0; // Release args allocas. TODO: This is a bad design
	return Endianness::toNative(m_builder, m_builder.CreateLoad(ret));
}

llvm::Value* Ext::query(evm_query_key _key)
{
	auto func = getQueryFunc(getModule());
	auto undef = llvm::UndefValue::get(Type::WordPtr);
	auto v = createCABICall(func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(_key), undef});

	switch (_key)
	{
	case EVM_ADDRESS:
	case EVM_CALLER:
	case EVM_ORIGIN:
	case EVM_COINBASE:
	{
		auto mask160 = llvm::APInt(160, -1, true).zext(256);
		v = Endianness::toNative(m_builder, v);
		v = m_builder.CreateAnd(v, mask160);
		break;
	}
	case EVM_GAS_LIMIT:
	case EVM_NUMBER:
	case EVM_TIMESTAMP:
	{
		// Use only 64-bit -- single word. The rest is uninitialized.
		// We could have mask 63 bits, but there is very little to gain in cost
		// of additional and operation.
		auto mask64 = llvm::APInt(256, std::numeric_limits<uint64_t>::max());
		v = m_builder.CreateAnd(v, mask64);
		break;
	}
	default:
		break;
	}

	return v;
}

llvm::Value* Ext::balance(llvm::Value* _address)
{
	auto func = getQueryFunc(getModule());
	auto address = Endianness::toBE(m_builder, _address);
	return createCABICall(func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_BALANCE), address});
}

llvm::Value* Ext::blockHash(llvm::Value* _number)
{
	auto func = getQueryFunc(getModule());
	// TODO: We can explicitly trunc the number to i64. The optimizer will know
	//       that we care only about these 64 bit, not all 256.
	auto hash = createCABICall(func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_BLOCKHASH), _number});
	return Endianness::toNative(m_builder, hash);
}

llvm::Value* Ext::create(llvm::Value* _endowment, llvm::Value* _initOff, llvm::Value* _initSize)
{
	auto ret = getArgAlloca();
	auto begin = m_memoryMan.getBytePtr(_initOff);
	auto size = m_builder.CreateTrunc(_initSize, Type::Size, "size");
	createCall(EnvFunc::create, {getRuntimeManager().getEnvPtr(), getRuntimeManager().getGasPtr(), byPtr(_endowment), begin, size, ret});
	llvm::Value* address = m_builder.CreateLoad(ret);
	address = Endianness::toNative(m_builder, address);
	return address;
}

llvm::Value* Ext::sha3(llvm::Value* _inOff, llvm::Value* _inSize)
{
	auto begin = m_memoryMan.getBytePtr(_inOff);
	auto size = m_builder.CreateTrunc(_inSize, Type::Size, "size");
	auto ret = getArgAlloca();
	createCall(EnvFunc::sha3, {begin, size, ret});
	llvm::Value* hash = m_builder.CreateLoad(ret);
	hash = Endianness::toNative(m_builder, hash);
	return hash;
}

MemoryRef Ext::extcode(llvm::Value* _addr)
{
	auto func = getQueryFunc(getModule());
	// TODO: We care only about 20 bytes here. Can we do it better?
	auto address = Endianness::toBE(m_builder, _addr);
	auto vPtr = createCABICall(func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_CODE_BY_ADDRESS), address}, false);
	auto memRefTy = getMemRefTy(getModule());
	auto memRefPtr = m_builder.CreateBitCast(vPtr, memRefTy->getPointerTo());
	auto memRef = m_builder.CreateLoad(memRefTy, memRefPtr, "memref");
	auto code = m_builder.CreateExtractValue(memRef, 0, "code");
	auto size = m_builder.CreateExtractValue(memRef, 1, "codesize");
	auto size256 = m_builder.CreateZExt(size, Type::Word);
	return {code, size256};
}

void Ext::log(llvm::Value* _memIdx, llvm::Value* _numBytes, llvm::ArrayRef<llvm::Value*> _topics)
{
	if (!m_topics)
	{
		InsertPointGuard g{m_builder};
		auto& entryBB = getMainFunction()->front();
		m_builder.SetInsertPoint(&entryBB, entryBB.begin());
		m_topics = m_builder.CreateAlloca(Type::Word, m_builder.getInt32(4), "topics");
	}

	auto begin = m_memoryMan.getBytePtr(_memIdx);
	auto size = m_builder.CreateTrunc(_numBytes, Type::Size, "size");

	for (size_t i = 0; i < _topics.size(); ++i)
	{
		auto t = Endianness::toBE(m_builder, _topics[i]);
		auto p = m_builder.CreateConstGEP1_32(m_topics, static_cast<unsigned>(i));
		m_builder.CreateStore(t, p);
	}

	auto func = getUpdateFunc(getModule());
	auto a = getArgAlloca();
	auto memRefTy = getMemRefTy(getModule());
	auto pMemRef = m_builder.CreateBitCast(a, memRefTy->getPointerTo());
	auto pData = m_builder.CreateConstGEP2_32(memRefTy, pMemRef, 0, 0, "log.data");
	m_builder.CreateStore(begin, pData);
	auto pSize = m_builder.CreateConstGEP2_32(memRefTy, pMemRef, 0, 1, "log.size");
	m_builder.CreateStore(size, pSize);

	auto b = getArgAlloca();
	pMemRef = m_builder.CreateBitCast(b, memRefTy->getPointerTo());
	pData = m_builder.CreateConstGEP2_32(memRefTy, pMemRef, 0, 0, "topics.data");
	m_builder.CreateStore(m_builder.CreateBitCast(m_topics, m_builder.getInt8PtrTy()), pData);
	pSize = m_builder.CreateConstGEP2_32(memRefTy, pMemRef, 0, 1, "topics.size");
	m_builder.CreateStore(m_builder.getInt64(_topics.size() * 32), pSize);

	createCABICall(func, {getRuntimeManager().getEnvPtr(), m_builder.getInt32(EVM_LOG), a, b});
}

llvm::Value* Ext::call(evm_call_kind _kind,
	                   llvm::Value* _gas,
	                   llvm::Value* _addr,
	                   llvm::Value* _value,
	                   llvm::Value* _inOff,
	                   llvm::Value* _inSize,
	                   llvm::Value* _outOff,
	                   llvm::Value* _outSize)
{
	auto memRefTy = getMemRefTy(getModule());
	auto gas = m_builder.CreateTrunc(_gas, Type::Size);
	auto addr = m_builder.CreateTrunc(_addr, m_builder.getIntNTy(160));
	addr = Endianness::toBE(m_builder, addr);
	auto inData = m_memoryMan.getBytePtr(_inOff);
	auto inSize = m_builder.CreateTrunc(_inSize, Type::Size);
	auto initMemRef = llvm::UndefValue::get(memRefTy);
	auto outData = m_memoryMan.getBytePtr(_outOff);
	auto outSize = m_builder.CreateTrunc(_outSize, Type::Size);
	auto out = m_builder.CreateInsertValue(initMemRef, outData, 0);
	out = m_builder.CreateInsertValue(out, outSize, 1);

	auto func = getCallFunc(getModule());
	return createCABICall(func, {getRuntimeManager().getEnvPtr(),
	                      m_builder.getInt32(_kind),
	                      gas,
	                      addr,
	                      _value,
	                      inData,
	                      inSize,
	                      out});
}

}
}
}
