#include "EmitContext.h"
#include "EmitFunctionContext.h"
#include "EmitModuleContext.h"
#include "LLVMJITPrivate.h"
#include "WAVM/IR/Operators.h"
#include "WAVM/IR/Types.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"

PUSH_DISABLE_WARNINGS_FOR_LLVM_HEADERS
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/AtomicOrdering.h>
POP_DISABLE_WARNINGS_FOR_LLVM_HEADERS

using namespace WAVM;
using namespace WAVM::IR;
using namespace WAVM::LLVMJIT;

// Bounds checks a sandboxed memory address + offset, and returns an offset relative to the memory
// base address that is guaranteed to be within the virtual address space allocated for the linear
// memory object.
static llvm::Value* getOffsetAndBoundedAddress(EmitContext& emitContext,
											   llvm::Value* address,
											   U32 offset)
{
	// zext the 32-bit address to 64-bits.
	// This is crucial for security, as LLVM will otherwise implicitly sign extend it to 64-bits in
	// the GEP below, interpreting it as a signed offset and allowing access to memory outside the
	// sandboxed memory range. There are no 'far addresses' in a 32 bit runtime.
	address = emitContext.irBuilder.CreateZExt(address, emitContext.llvmContext.i64Type);

	// Add the offset to the byte index.
	if(offset)
	{
		address = emitContext.irBuilder.CreateAdd(
			address,
			emitContext.irBuilder.CreateZExt(emitLiteral(emitContext.llvmContext, offset),
											 emitContext.llvmContext.i64Type));
	}

	// If HAS_64BIT_ADDRESS_SPACE, the memory has enough virtual address space allocated to ensure
	// that any 32-bit byte index + 32-bit offset will fall within the virtual address sandbox, so
	// no explicit bounds check is necessary.

	return address;
}

llvm::Value* EmitFunctionContext::coerceAddressToPointer(llvm::Value* boundedAddress,
														 llvm::Type* memoryType,
														 Uptr memoryIndex)
{
	llvm::Value* memoryBasePointer = irBuilder.CreateLoad(memoryBasePointerVariables[memoryIndex]);
	llvm::Value* bytePointer = irBuilder.CreateInBoundsGEP(memoryBasePointer, boundedAddress);

	// Cast the pointer to the appropriate type.
	return irBuilder.CreatePointerCast(bytePointer, memoryType->getPointerTo());
}

//
// Memory size operators
// These just call out to wavmIntrinsics.growMemory/currentMemory, passing a pointer to the default
// memory for the module.
//

void EmitFunctionContext::memory_grow(MemoryImm imm)
{
	llvm::Value* deltaNumPages = pop();
	ValueVector previousNumPages = emitRuntimeIntrinsic(
		"memory.grow",
		FunctionType(TypeTuple(ValueType::i32),
					 TypeTuple({ValueType::i32, inferValueType<Uptr>()}),
					 IR::CallingConvention::intrinsic),
		{deltaNumPages,
		 getMemoryIdFromOffset(llvmContext, moduleContext.memoryOffsets[imm.memoryIndex])});
	WAVM_ASSERT(previousNumPages.size() == 1);
	push(previousNumPages[0]);
}
void EmitFunctionContext::memory_size(MemoryImm imm)
{
	ValueVector currentNumPages = emitRuntimeIntrinsic(
		"memory.size",
		FunctionType(TypeTuple(ValueType::i32),
					 TypeTuple(inferValueType<Uptr>()),
					 IR::CallingConvention::intrinsic),
		{getMemoryIdFromOffset(llvmContext, moduleContext.memoryOffsets[imm.memoryIndex])});
	WAVM_ASSERT(currentNumPages.size() == 1);
	push(currentNumPages[0]);
}

//
// Memory bulk operators.
//

void EmitFunctionContext::memory_init(DataSegmentAndMemImm imm)
{
	auto numBytes = pop();
	auto sourceOffset = pop();
	auto destAddress = pop();
	emitRuntimeIntrinsic(
		"memory.init",
		FunctionType({},
					 TypeTuple({ValueType::i32,
								ValueType::i32,
								ValueType::i32,
								inferValueType<Uptr>(),
								inferValueType<Uptr>(),
								inferValueType<Uptr>()}),
					 IR::CallingConvention::intrinsic),
		{destAddress,
		 sourceOffset,
		 numBytes,
		 moduleContext.instanceId,
		 getMemoryIdFromOffset(llvmContext, moduleContext.memoryOffsets[imm.memoryIndex]),
		 emitLiteral(llvmContext, imm.dataSegmentIndex)});
}

void EmitFunctionContext::data_drop(DataSegmentImm imm)
{
	emitRuntimeIntrinsic(
		"data.drop",
		FunctionType({},
					 TypeTuple({inferValueType<Uptr>(), inferValueType<Uptr>()}),
					 IR::CallingConvention::intrinsic),
		{moduleContext.instanceId, emitLiteral(llvmContext, imm.dataSegmentIndex)});
}

static void emitLoop(EmitFunctionContext& functionContext,
					 llvm::BasicBlock* outgoingBlock,
					 llvm::Value* beginIndex,
					 llvm::Value* endIndex,
					 bool reverse,
					 std::function<void(llvm::Value*)>&& emitBody)
{
	llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;

	// Create a loop head block.
	llvm::BasicBlock* incomingBlock = irBuilder.GetInsertBlock();
	llvm::BasicBlock* loopHeadBlock = llvm::BasicBlock::Create(
		functionContext.llvmContext, "loopHead", functionContext.function);
	irBuilder.CreateBr(loopHeadBlock);
	irBuilder.SetInsertPoint(loopHeadBlock);

	llvm::PHINode* indexPHI = irBuilder.CreatePHI(functionContext.llvmContext.iptrType, 2);

	// Emit the loop condition.
	llvm::BasicBlock* loopBodyBlock = llvm::BasicBlock::Create(
		functionContext.llvmContext, "loopBody", functionContext.function);
	if(reverse)
	{
		indexPHI->addIncoming(endIndex, incomingBlock);
		llvm::Value* indexNotEqualBegin = irBuilder.CreateICmpNE(indexPHI, beginIndex);
		irBuilder.CreateCondBr(indexNotEqualBegin, loopBodyBlock, outgoingBlock);
	}
	else
	{
		indexPHI->addIncoming(beginIndex, incomingBlock);
		llvm::Value* indexLessThanEnd = irBuilder.CreateICmpULT(indexPHI, endIndex);
		irBuilder.CreateCondBr(indexLessThanEnd, loopBodyBlock, outgoingBlock);
	}

	irBuilder.SetInsertPoint(loopBodyBlock);

	// For reverse loops, update the index between checking the condition and the loop body.
	llvm::Value* index = indexPHI;
	if(reverse)
	{
		index = irBuilder.CreateSub(
			indexPHI, llvm::ConstantInt::get(functionContext.llvmContext.iptrType, 1));
		indexPHI->addIncoming(index, loopBodyBlock);
	}

	// Emit the loop body.
	emitBody(index);

	// For forward loops, update the index between the loop body and branching back to the loop head
	// block where the condition is checked.
	if(!reverse)
	{
		llvm::Value* indexPlusOne = irBuilder.CreateAdd(
			indexPHI, llvm::ConstantInt::get(functionContext.llvmContext.iptrType, 1));
		indexPHI->addIncoming(indexPlusOne, loopBodyBlock);
	}
	irBuilder.CreateBr(loopHeadBlock);
}

static void emitMemoryCopyLoop(EmitFunctionContext& functionContext,
							   llvm::BasicBlock* outgoingBlock,
							   llvm::Value* sourcePointer,
							   llvm::Value* destPointer,
							   llvm::Value* numBytesUptr,
							   bool reverse)
{
	emitLoop(functionContext,
			 outgoingBlock,
			 llvm::ConstantInt::getNullValue(functionContext.llvmContext.iptrType),
			 numBytesUptr,
			 reverse,
			 [&functionContext, sourcePointer, destPointer](llvm::Value* index) {
				 llvm::LoadInst* load = functionContext.irBuilder.CreateLoad(
					 functionContext.irBuilder.CreateInBoundsGEP(sourcePointer, {index}));
				 load->setAlignment(1);
				 load->setVolatile(true);

				 llvm::StoreInst* store = functionContext.irBuilder.CreateStore(
					 load, functionContext.irBuilder.CreateInBoundsGEP(destPointer, {index}));
				 store->setAlignment(1);
				 store->setVolatile(true);
			 });
}

void EmitFunctionContext::memory_copy(MemoryCopyImm imm)
{
	llvm::Value* numBytes = pop();
	llvm::Value* sourceAddress = pop();
	llvm::Value* destAddress = pop();

	llvm::Value* sourceBoundedAddress = getOffsetAndBoundedAddress(*this, sourceAddress, 0);
	llvm::Value* destBoundedAddress = getOffsetAndBoundedAddress(*this, destAddress, 0);

	llvm::Value* sourcePointer
		= coerceAddressToPointer(sourceBoundedAddress, llvmContext.i8Type, imm.sourceMemoryIndex);
	llvm::Value* destPointer
		= coerceAddressToPointer(destBoundedAddress, llvmContext.i8Type, imm.destMemoryIndex);

	llvm::Value* numBytesUptr = irBuilder.CreateZExt(numBytes, llvmContext.iptrType);

	// Branch to a forward or reverse basic block depending on the order of the addresses
	// (disregarding that they may be addressing to different memory objects).
	llvm::BasicBlock* reverseBlock
		= llvm::BasicBlock::Create(llvmContext, "memoryCopyReverse", function);
	llvm::BasicBlock* forwardBlock
		= llvm::BasicBlock::Create(llvmContext, "memoryCopyForward", function);
	llvm::BasicBlock* joinBlock = llvm::BasicBlock::Create(llvmContext, "memoryCopyJoin", function);
	llvm::Value* sourceAddressIsLessThanDestAddress
		= irBuilder.CreateICmpULT(sourceBoundedAddress, destBoundedAddress);
	irBuilder.CreateCondBr(sourceAddressIsLessThanDestAddress, reverseBlock, forwardBlock);
	irBuilder.SetInsertPoint(reverseBlock);

	// Emit the reverse case: a simple byte-wise copy loop.
	// (on x86 this is faster than the "std; rep movsb; cld" variant of the forward case.
	emitMemoryCopyLoop(*this, joinBlock, sourcePointer, destPointer, numBytesUptr, true);

	// Emit the forward case.
	forwardBlock->moveAfter(irBuilder.GetInsertBlock());
	irBuilder.SetInsertPoint(forwardBlock);

	if(moduleContext.targetArch == llvm::Triple::x86
	   || moduleContext.targetArch == llvm::Triple::x86_64)
	{
		// On x86, use "rep movsb" to do forward copies.
		llvm::FunctionType* inlineAssemblySig = llvm::FunctionType::get(
			llvm::StructType::get(
				llvmContext, {llvmContext.i8PtrType, llvmContext.i8PtrType, llvmContext.iptrType}),
			{llvmContext.i8PtrType, llvmContext.i8PtrType, llvmContext.iptrType},
			false);
		llvm::InlineAsm* forwardInlineAssembly
			= llvm::InlineAsm::get(inlineAssemblySig,
								   "rep movsb",
								   "={di},={si},={cx},0,1,2,~{memory},~{dirflag},~{fpsr},~{flags}",
								   true,
								   false);
		irBuilder.CreateCall(forwardInlineAssembly, {destPointer, sourcePointer, numBytesUptr});
		irBuilder.CreateBr(joinBlock);
	}
	else
	{
		// Otherwise, emit a simple byte-wise copy loop.
		emitMemoryCopyLoop(*this, joinBlock, sourcePointer, destPointer, numBytesUptr, false);
	}

	joinBlock->moveAfter(irBuilder.GetInsertBlock());
	irBuilder.SetInsertPoint(joinBlock);
}

void EmitFunctionContext::memory_fill(MemoryImm imm)
{
	llvm::Value* numBytes = pop();
	llvm::Value* value = pop();
	llvm::Value* destAddress = pop();

	llvm::Value* destBoundedAddress = getOffsetAndBoundedAddress(*this, destAddress, 0);
	llvm::Value* destPointer
		= coerceAddressToPointer(destBoundedAddress, llvmContext.i8Type, imm.memoryIndex);

	llvm::Value* numBytesUptr = irBuilder.CreateZExt(numBytes, llvmContext.iptrType);

	if(moduleContext.targetArch == llvm::Triple::x86
	   || moduleContext.targetArch == llvm::Triple::x86_64)
	{
		// On x86, use "rep stosb".
		llvm::FunctionType* inlineAssemblySig = llvm::FunctionType::get(
			llvm::StructType::get(
				llvmContext, {llvmContext.i8PtrType, llvmContext.i8Type, llvmContext.iptrType}),
			{llvmContext.i8PtrType, llvmContext.i8Type, llvmContext.iptrType},
			false);
		llvm::InlineAsm* inlineAssembly
			= llvm::InlineAsm::get(inlineAssemblySig,
								   "rep stosb",
								   "={di},={al},={cx},0,1,2,~{memory},~{dirflag},~{fpsr},~{flags}",
								   true,
								   false);

		irBuilder.CreateCall(
			inlineAssembly,
			{destPointer, irBuilder.CreateTrunc(value, llvmContext.i8Type), numBytesUptr});
	}
	else
	{
		// On non-x86 architectures, just emit a simple byte-wise memory fill loop.
		llvm::Value* valueI8 = irBuilder.CreateTrunc(value, llvmContext.i8Type);

		llvm::BasicBlock* endBlock
			= llvm::BasicBlock::Create(llvmContext, "memoryFillEnd", function);
		emitLoop(*this,
				 endBlock,
				 llvm::ConstantInt::getNullValue(llvmContext.iptrType),
				 numBytesUptr,
				 false,
				 [&](llvm::Value* index) {
					 llvm::StoreInst* store = irBuilder.CreateStore(
						 valueI8, irBuilder.CreateInBoundsGEP(destPointer, {index}));
					 store->setAlignment(1);
					 store->setVolatile(true);
				 });

		endBlock->moveAfter(irBuilder.GetInsertBlock());
		irBuilder.SetInsertPoint(endBlock);
	}
}

//
// Load/store operators
//

#define EMIT_LOAD_OP(destType, name, llvmMemoryType, naturalAlignmentLog2, conversionOp)           \
	void EmitFunctionContext::name(LoadOrStoreImm<naturalAlignmentLog2> imm)                       \
	{                                                                                              \
		auto address = pop();                                                                      \
		auto boundedAddress = getOffsetAndBoundedAddress(*this, address, imm.offset);              \
		auto pointer = coerceAddressToPointer(boundedAddress, llvmMemoryType, imm.memoryIndex);    \
		auto load = irBuilder.CreateLoad(pointer);                                                 \
		/* Don't trust the alignment hint provided by the WebAssembly code, since the load can't   \
		 * trap if it's wrong. */                                                                  \
		load->setAlignment(1);                                                                     \
		load->setVolatile(true);                                                                   \
		push(conversionOp(load, destType));                                                        \
	}
#define EMIT_STORE_OP(name, llvmMemoryType, naturalAlignmentLog2, conversionOp)                    \
	void EmitFunctionContext::name(LoadOrStoreImm<naturalAlignmentLog2> imm)                       \
	{                                                                                              \
		auto value = pop();                                                                        \
		auto address = pop();                                                                      \
		auto boundedAddress = getOffsetAndBoundedAddress(*this, address, imm.offset);              \
		auto pointer = coerceAddressToPointer(boundedAddress, llvmMemoryType, imm.memoryIndex);    \
		auto memoryValue = conversionOp(value, llvmMemoryType);                                    \
		auto store = irBuilder.CreateStore(memoryValue, pointer);                                  \
		store->setVolatile(true);                                                                  \
		/* Don't trust the alignment hint provided by the WebAssembly code, since the store can't  \
		 * trap if it's wrong. */                                                                  \
		store->setAlignment(1);                                                                    \
	}

EMIT_LOAD_OP(llvmContext.i32Type, i32_load8_s, llvmContext.i8Type, 0, sext)
EMIT_LOAD_OP(llvmContext.i32Type, i32_load8_u, llvmContext.i8Type, 0, zext)
EMIT_LOAD_OP(llvmContext.i32Type, i32_load16_s, llvmContext.i16Type, 1, sext)
EMIT_LOAD_OP(llvmContext.i32Type, i32_load16_u, llvmContext.i16Type, 1, zext)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load8_s, llvmContext.i8Type, 0, sext)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load8_u, llvmContext.i8Type, 0, zext)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load16_s, llvmContext.i16Type, 1, sext)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load16_u, llvmContext.i16Type, 1, zext)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load32_s, llvmContext.i32Type, 2, sext)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load32_u, llvmContext.i32Type, 2, zext)

EMIT_LOAD_OP(llvmContext.i32Type, i32_load, llvmContext.i32Type, 2, identity)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load, llvmContext.i64Type, 3, identity)
EMIT_LOAD_OP(llvmContext.f32Type, f32_load, llvmContext.f32Type, 2, identity)
EMIT_LOAD_OP(llvmContext.f64Type, f64_load, llvmContext.f64Type, 3, identity)

EMIT_STORE_OP(i32_store8, llvmContext.i8Type, 0, trunc)
EMIT_STORE_OP(i64_store8, llvmContext.i8Type, 0, trunc)
EMIT_STORE_OP(i32_store16, llvmContext.i16Type, 1, trunc)
EMIT_STORE_OP(i64_store16, llvmContext.i16Type, 1, trunc)
EMIT_STORE_OP(i32_store, llvmContext.i32Type, 2, trunc)
EMIT_STORE_OP(i64_store32, llvmContext.i32Type, 2, trunc)
EMIT_STORE_OP(i64_store, llvmContext.i64Type, 3, identity)
EMIT_STORE_OP(f32_store, llvmContext.f32Type, 2, identity)
EMIT_STORE_OP(f64_store, llvmContext.f64Type, 3, identity)

EMIT_STORE_OP(v128_store, value->getType(), 4, identity)
EMIT_LOAD_OP(llvmContext.i64x2Type, v128_load, llvmContext.i64x2Type, 4, identity)

EMIT_LOAD_OP(llvmContext.i8x16Type, v8x16_load_splat, llvmContext.i8Type, 0, splat<16>)
EMIT_LOAD_OP(llvmContext.i16x8Type, v16x8_load_splat, llvmContext.i16Type, 1, splat<8>)
EMIT_LOAD_OP(llvmContext.i32x4Type, v32x4_load_splat, llvmContext.i32Type, 2, splat<4>)
EMIT_LOAD_OP(llvmContext.i64x2Type, v64x2_load_splat, llvmContext.i64Type, 3, splat<2>)

EMIT_LOAD_OP(llvmContext.i16x8Type, i16x8_load8x8_s, llvmContext.i8x8Type, 3, sext)
EMIT_LOAD_OP(llvmContext.i16x8Type, i16x8_load8x8_u, llvmContext.i8x8Type, 3, zext)
EMIT_LOAD_OP(llvmContext.i32x4Type, i32x4_load16x4_s, llvmContext.i16x4Type, 3, sext)
EMIT_LOAD_OP(llvmContext.i32x4Type, i32x4_load16x4_u, llvmContext.i16x4Type, 3, zext)
EMIT_LOAD_OP(llvmContext.i64x2Type, i64x2_load32x2_s, llvmContext.i32x2Type, 3, sext)
EMIT_LOAD_OP(llvmContext.i64x2Type, i64x2_load32x2_u, llvmContext.i32x2Type, 3, zext)

static void emitLoadInterleaved(EmitFunctionContext& functionContext,
								llvm::Type* llvmValueType,
								llvm::Intrinsic::ID aarch64IntrinsicID,
								U8 alignmentLog2,
								U32 offset,
								Uptr memoryIndex,
								U32 numVectors,
								U32 numLanes)
{
	static constexpr U32 maxVectors = 4;
	static constexpr U32 maxLanes = 16;
	WAVM_ASSERT(numVectors <= maxVectors);
	WAVM_ASSERT(numLanes <= maxLanes);

	auto address = functionContext.pop();
	auto boundedAddress = getOffsetAndBoundedAddress(functionContext, address, offset);
	auto pointer
		= functionContext.coerceAddressToPointer(boundedAddress, llvmValueType, memoryIndex);
	if(functionContext.moduleContext.targetArch == llvm::Triple::aarch64)
	{
		auto results = functionContext.callLLVMIntrinsic(
			{llvmValueType, llvmValueType->getPointerTo()}, aarch64IntrinsicID, {pointer});
		for(U32 vectorIndex = 0; vectorIndex < numVectors; ++vectorIndex)
		{
			functionContext.push(
				functionContext.irBuilder.CreateExtractValue(results, vectorIndex));
		}
	}
	else
	{
		llvm::Value* loads[maxVectors];
		for(U32 vectorIndex = 0; vectorIndex < numVectors; ++vectorIndex)
		{
			auto load
				= functionContext.irBuilder.CreateLoad(functionContext.irBuilder.CreateInBoundsGEP(
					pointer, {emitLiteral(functionContext.llvmContext, U32(vectorIndex))}));
			/* Don't trust the alignment hint provided by the WebAssembly code, since the load
			 * can't trap if it's wrong. */
			load->setAlignment(1);
			load->setVolatile(true);
			loads[vectorIndex] = load;
		}
		for(U32 vectorIndex = 0; vectorIndex < numVectors; ++vectorIndex)
		{
			llvm::Value* deinterleavedVector = llvm::UndefValue::get(llvmValueType);
			for(U32 laneIndex = 0; laneIndex < numLanes; ++laneIndex)
			{
				const Uptr interleavedElementIndex = laneIndex * numVectors + vectorIndex;
				deinterleavedVector = functionContext.irBuilder.CreateInsertElement(
					deinterleavedVector,
					functionContext.irBuilder.CreateExtractElement(
						loads[interleavedElementIndex / numLanes],
						interleavedElementIndex % numLanes),
					laneIndex);
			}
			functionContext.push(deinterleavedVector);
		}
	}
}

static void emitStoreInterleaved(EmitFunctionContext& functionContext,
								 llvm::Type* llvmValueType,
								 llvm::Intrinsic::ID aarch64IntrinsicID,
								 U8 alignmentLog2,
								 U32 offset,
								 Uptr memoryIndex,
								 U32 numVectors,
								 U32 numLanes)
{
	static constexpr U32 maxVectors = 4;
	WAVM_ASSERT(numVectors <= 4);

	llvm::Value* values[maxVectors];
	for(U32 vectorIndex = 0; vectorIndex < numVectors; ++vectorIndex)
	{
		values[numVectors - vectorIndex - 1]
			= functionContext.irBuilder.CreateBitCast(functionContext.pop(), llvmValueType);
	}
	auto address = functionContext.pop();
	auto boundedAddress = getOffsetAndBoundedAddress(functionContext, address, offset);
	auto pointer
		= functionContext.coerceAddressToPointer(boundedAddress, llvmValueType, memoryIndex);
	if(functionContext.moduleContext.targetArch == llvm::Triple::aarch64)
	{
		llvm::Value* args[maxVectors + 1];
		for(U32 vectorIndex = 0; vectorIndex < numVectors; ++vectorIndex)
		{
			args[vectorIndex] = values[vectorIndex];
			args[numVectors] = pointer;
		}
		functionContext.callLLVMIntrinsic({llvmValueType, llvmValueType->getPointerTo()},
										  aarch64IntrinsicID,
										  llvm::ArrayRef<llvm::Value*>(args, numVectors + 1));
	}
	else
	{
		for(U32 vectorIndex = 0; vectorIndex < numVectors; ++vectorIndex)
		{
			llvm::Value* interleavedVector = llvm::UndefValue::get(llvmValueType);
			for(U32 laneIndex = 0; laneIndex < numLanes; ++laneIndex)
			{
				const Uptr interleavedElementIndex = vectorIndex * numLanes + laneIndex;
				const Uptr deinterleavedVectorIndex = interleavedElementIndex % numVectors;
				const Uptr deinterleavedLaneIndex = interleavedElementIndex / numVectors;
				interleavedVector = functionContext.irBuilder.CreateInsertElement(
					interleavedVector,
					functionContext.irBuilder.CreateExtractElement(values[deinterleavedVectorIndex],
																   deinterleavedLaneIndex),
					laneIndex);
			}
			auto store = functionContext.irBuilder.CreateStore(
				interleavedVector,
				functionContext.irBuilder.CreateInBoundsGEP(
					pointer, {emitLiteral(functionContext.llvmContext, U32(vectorIndex))}));
			store->setVolatile(true);
			store->setAlignment(1);
		}
	}
}

#define EMIT_LOAD_INTERLEAVED_OP(name, llvmValueType, naturalAlignmentLog2, numVectors, numLanes)  \
	void EmitFunctionContext::name(LoadOrStoreImm<naturalAlignmentLog2> imm)                       \
	{                                                                                              \
		emitLoadInterleaved(*this,                                                                 \
							llvmValueType,                                                         \
							llvm::Intrinsic::aarch64_neon_ld##numVectors,                          \
							imm.alignmentLog2,                                                     \
							imm.offset,                                                            \
							imm.memoryIndex,                                                       \
							numVectors,                                                            \
							numLanes);                                                             \
	}

#define EMIT_STORE_INTERLEAVED_OP(name, llvmValueType, naturalAlignmentLog2, numVectors, numLanes) \
	void EmitFunctionContext::name(LoadOrStoreImm<naturalAlignmentLog2> imm)                       \
	{                                                                                              \
		emitStoreInterleaved(*this,                                                                \
							 llvmValueType,                                                        \
							 llvm::Intrinsic::aarch64_neon_st##numVectors,                         \
							 imm.alignmentLog2,                                                    \
							 imm.offset,                                                           \
							 imm.memoryIndex,                                                      \
							 numVectors,                                                           \
							 numLanes);                                                            \
	}

EMIT_LOAD_INTERLEAVED_OP(v8x16_load_interleaved_2, llvmContext.i8x16Type, 4, 2, 16)
EMIT_LOAD_INTERLEAVED_OP(v8x16_load_interleaved_3, llvmContext.i8x16Type, 4, 3, 16)
EMIT_LOAD_INTERLEAVED_OP(v8x16_load_interleaved_4, llvmContext.i8x16Type, 4, 4, 16)
EMIT_LOAD_INTERLEAVED_OP(v16x8_load_interleaved_2, llvmContext.i16x8Type, 4, 2, 8)
EMIT_LOAD_INTERLEAVED_OP(v16x8_load_interleaved_3, llvmContext.i16x8Type, 4, 3, 8)
EMIT_LOAD_INTERLEAVED_OP(v16x8_load_interleaved_4, llvmContext.i16x8Type, 4, 4, 8)
EMIT_LOAD_INTERLEAVED_OP(v32x4_load_interleaved_2, llvmContext.i32x4Type, 4, 2, 4)
EMIT_LOAD_INTERLEAVED_OP(v32x4_load_interleaved_3, llvmContext.i32x4Type, 4, 3, 4)
EMIT_LOAD_INTERLEAVED_OP(v32x4_load_interleaved_4, llvmContext.i32x4Type, 4, 4, 4)
EMIT_LOAD_INTERLEAVED_OP(v64x2_load_interleaved_2, llvmContext.i64x2Type, 4, 2, 2)
EMIT_LOAD_INTERLEAVED_OP(v64x2_load_interleaved_3, llvmContext.i64x2Type, 4, 3, 2)
EMIT_LOAD_INTERLEAVED_OP(v64x2_load_interleaved_4, llvmContext.i64x2Type, 4, 4, 2)

EMIT_STORE_INTERLEAVED_OP(v8x16_store_interleaved_2, llvmContext.i8x16Type, 4, 2, 16)
EMIT_STORE_INTERLEAVED_OP(v8x16_store_interleaved_3, llvmContext.i8x16Type, 4, 3, 16)
EMIT_STORE_INTERLEAVED_OP(v8x16_store_interleaved_4, llvmContext.i8x16Type, 4, 4, 16)
EMIT_STORE_INTERLEAVED_OP(v16x8_store_interleaved_2, llvmContext.i16x8Type, 4, 2, 8)
EMIT_STORE_INTERLEAVED_OP(v16x8_store_interleaved_3, llvmContext.i16x8Type, 4, 3, 8)
EMIT_STORE_INTERLEAVED_OP(v16x8_store_interleaved_4, llvmContext.i16x8Type, 4, 4, 8)
EMIT_STORE_INTERLEAVED_OP(v32x4_store_interleaved_2, llvmContext.i32x4Type, 4, 2, 4)
EMIT_STORE_INTERLEAVED_OP(v32x4_store_interleaved_3, llvmContext.i32x4Type, 4, 3, 4)
EMIT_STORE_INTERLEAVED_OP(v32x4_store_interleaved_4, llvmContext.i32x4Type, 4, 4, 4)
EMIT_STORE_INTERLEAVED_OP(v64x2_store_interleaved_2, llvmContext.i64x2Type, 4, 2, 2)
EMIT_STORE_INTERLEAVED_OP(v64x2_store_interleaved_3, llvmContext.i64x2Type, 4, 3, 2)
EMIT_STORE_INTERLEAVED_OP(v64x2_store_interleaved_4, llvmContext.i64x2Type, 4, 4, 2)

void EmitFunctionContext::trapIfMisalignedAtomic(llvm::Value* address, U32 alignmentLog2)
{
	if(alignmentLog2 > 0)
	{
		emitConditionalTrapIntrinsic(
			irBuilder.CreateICmpNE(
				llvmContext.typedZeroConstants[(Uptr)ValueType::i64],
				irBuilder.CreateAnd(address,
									emitLiteral(llvmContext, (U64(1) << alignmentLog2) - 1))),
			"misalignedAtomicTrap",
			FunctionType(TypeTuple{}, TypeTuple{ValueType::i64}, IR::CallingConvention::intrinsic),
			{address});
	}
}

void EmitFunctionContext::atomic_notify(AtomicLoadOrStoreImm<2> imm)
{
	llvm::Value* numWaiters = pop();
	llvm::Value* address = pop();
	llvm::Value* boundedAddress = getOffsetAndBoundedAddress(*this, address, imm.offset);
	trapIfMisalignedAtomic(boundedAddress, imm.alignmentLog2);
	push(emitRuntimeIntrinsic(
		"atomic_notify",
		FunctionType(TypeTuple{ValueType::i32},
					 TypeTuple{ValueType::i32, ValueType::i32, ValueType::i64},
					 IR::CallingConvention::intrinsic),
		{address,
		 numWaiters,
		 getMemoryIdFromOffset(llvmContext, moduleContext.memoryOffsets[imm.memoryIndex])})[0]);
}
void EmitFunctionContext::i32_atomic_wait(AtomicLoadOrStoreImm<2> imm)
{
	llvm::Value* timeout = pop();
	llvm::Value* expectedValue = pop();
	llvm::Value* address = pop();
	llvm::Value* boundedAddress = getOffsetAndBoundedAddress(*this, address, imm.offset);
	trapIfMisalignedAtomic(boundedAddress, imm.alignmentLog2);
	push(emitRuntimeIntrinsic(
		"atomic_wait_i32",
		FunctionType(
			TypeTuple{ValueType::i32},
			TypeTuple{ValueType::i32, ValueType::i32, ValueType::i64, inferValueType<Uptr>()},
			IR::CallingConvention::intrinsic),
		{address,
		 expectedValue,
		 timeout,
		 getMemoryIdFromOffset(llvmContext, moduleContext.memoryOffsets[imm.memoryIndex])})[0]);
}
void EmitFunctionContext::i64_atomic_wait(AtomicLoadOrStoreImm<3> imm)
{
	llvm::Value* timeout = pop();
	llvm::Value* expectedValue = pop();
	llvm::Value* address = pop();
	llvm::Value* boundedAddress = getOffsetAndBoundedAddress(*this, address, imm.offset);
	trapIfMisalignedAtomic(boundedAddress, imm.alignmentLog2);
	push(emitRuntimeIntrinsic(
		"atomic_wait_i64",
		FunctionType(
			TypeTuple{ValueType::i32},
			TypeTuple{ValueType::i32, ValueType::i64, ValueType::i64, inferValueType<Uptr>()},
			IR::CallingConvention::intrinsic),
		{address,
		 expectedValue,
		 timeout,
		 getMemoryIdFromOffset(llvmContext, moduleContext.memoryOffsets[imm.memoryIndex])})[0]);
}

void EmitFunctionContext::atomic_fence(AtomicFenceImm imm)
{
	switch(imm.order)
	{
	case MemoryOrder::sequentiallyConsistent:
		irBuilder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
		break;
	default: WAVM_UNREACHABLE();
	};
}

#define EMIT_ATOMIC_LOAD_OP(valueTypeId, name, llvmMemoryType, naturalAlignmentLog2, memToValue)   \
	void EmitFunctionContext::valueTypeId##_##name(AtomicLoadOrStoreImm<naturalAlignmentLog2> imm) \
	{                                                                                              \
		auto address = pop();                                                                      \
		auto boundedAddress = getOffsetAndBoundedAddress(*this, address, imm.offset);              \
		trapIfMisalignedAtomic(boundedAddress, naturalAlignmentLog2);                              \
		auto pointer = coerceAddressToPointer(boundedAddress, llvmMemoryType, imm.memoryIndex);    \
		auto load = irBuilder.CreateLoad(pointer);                                                 \
		load->setAlignment(1 << imm.alignmentLog2);                                                \
		load->setVolatile(true);                                                                   \
		load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);                             \
		push(memToValue(load, asLLVMType(llvmContext, ValueType::valueTypeId)));                   \
	}
#define EMIT_ATOMIC_STORE_OP(valueTypeId, name, llvmMemoryType, naturalAlignmentLog2, valueToMem)  \
	void EmitFunctionContext::valueTypeId##_##name(AtomicLoadOrStoreImm<naturalAlignmentLog2> imm) \
	{                                                                                              \
		auto value = pop();                                                                        \
		auto address = pop();                                                                      \
		auto boundedAddress = getOffsetAndBoundedAddress(*this, address, imm.offset);              \
		trapIfMisalignedAtomic(boundedAddress, naturalAlignmentLog2);                              \
		auto pointer = coerceAddressToPointer(boundedAddress, llvmMemoryType, imm.memoryIndex);    \
		auto memoryValue = valueToMem(value, llvmMemoryType);                                      \
		auto store = irBuilder.CreateStore(memoryValue, pointer);                                  \
		store->setVolatile(true);                                                                  \
		store->setAlignment(1 << imm.alignmentLog2);                                               \
		store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);                            \
	}
EMIT_ATOMIC_LOAD_OP(i32, atomic_load, llvmContext.i32Type, 2, identity)
EMIT_ATOMIC_LOAD_OP(i64, atomic_load, llvmContext.i64Type, 3, identity)

EMIT_ATOMIC_LOAD_OP(i32, atomic_load8_u, llvmContext.i8Type, 0, zext)
EMIT_ATOMIC_LOAD_OP(i32, atomic_load16_u, llvmContext.i16Type, 1, zext)
EMIT_ATOMIC_LOAD_OP(i64, atomic_load8_u, llvmContext.i8Type, 0, zext)
EMIT_ATOMIC_LOAD_OP(i64, atomic_load16_u, llvmContext.i16Type, 1, zext)
EMIT_ATOMIC_LOAD_OP(i64, atomic_load32_u, llvmContext.i32Type, 2, zext)

EMIT_ATOMIC_STORE_OP(i32, atomic_store, llvmContext.i32Type, 2, identity)
EMIT_ATOMIC_STORE_OP(i64, atomic_store, llvmContext.i64Type, 3, identity)

EMIT_ATOMIC_STORE_OP(i32, atomic_store8, llvmContext.i8Type, 0, trunc)
EMIT_ATOMIC_STORE_OP(i32, atomic_store16, llvmContext.i16Type, 1, trunc)
EMIT_ATOMIC_STORE_OP(i64, atomic_store8, llvmContext.i8Type, 0, trunc)
EMIT_ATOMIC_STORE_OP(i64, atomic_store16, llvmContext.i16Type, 1, trunc)
EMIT_ATOMIC_STORE_OP(i64, atomic_store32, llvmContext.i32Type, 2, trunc)

#define EMIT_ATOMIC_CMPXCHG(                                                                       \
	valueTypeId, name, llvmMemoryType, alignmentLog2, memToValue, valueToMem)                      \
	void EmitFunctionContext::valueTypeId##_##name(AtomicLoadOrStoreImm<alignmentLog2> imm)        \
	{                                                                                              \
		auto replacementValue = valueToMem(pop(), llvmMemoryType);                                 \
		auto expectedValue = valueToMem(pop(), llvmMemoryType);                                    \
		auto address = pop();                                                                      \
		auto boundedAddress = getOffsetAndBoundedAddress(*this, address, imm.offset);              \
		trapIfMisalignedAtomic(boundedAddress, alignmentLog2);                                     \
		auto pointer = coerceAddressToPointer(boundedAddress, llvmMemoryType, imm.memoryIndex);    \
		auto atomicCmpXchg                                                                         \
			= irBuilder.CreateAtomicCmpXchg(pointer,                                               \
											expectedValue,                                         \
											replacementValue,                                      \
											llvm::AtomicOrdering::SequentiallyConsistent,          \
											llvm::AtomicOrdering::SequentiallyConsistent);         \
		atomicCmpXchg->setVolatile(true);                                                          \
		auto previousValue = irBuilder.CreateExtractValue(atomicCmpXchg, {0});                     \
		push(memToValue(previousValue, asLLVMType(llvmContext, ValueType::valueTypeId)));          \
	}

EMIT_ATOMIC_CMPXCHG(i32, atomic_rmw8_cmpxchg_u, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_CMPXCHG(i32, atomic_rmw16_cmpxchg_u, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_CMPXCHG(i32, atomic_rmw_cmpxchg, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_CMPXCHG(i64, atomic_rmw8_cmpxchg_u, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_CMPXCHG(i64, atomic_rmw16_cmpxchg_u, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_CMPXCHG(i64, atomic_rmw32_cmpxchg_u, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_CMPXCHG(i64, atomic_rmw_cmpxchg, llvmContext.i64Type, 3, identity, identity)

#define EMIT_ATOMIC_RMW(                                                                           \
	valueTypeId, name, rmwOpId, llvmMemoryType, alignmentLog2, memToValue, valueToMem)             \
	void EmitFunctionContext::valueTypeId##_##name(AtomicLoadOrStoreImm<alignmentLog2> imm)        \
	{                                                                                              \
		auto value = valueToMem(pop(), llvmMemoryType);                                            \
		auto address = pop();                                                                      \
		auto boundedAddress = getOffsetAndBoundedAddress(*this, address, imm.offset);              \
		trapIfMisalignedAtomic(boundedAddress, alignmentLog2);                                     \
		auto pointer = coerceAddressToPointer(boundedAddress, llvmMemoryType, imm.memoryIndex);    \
		auto atomicRMW = irBuilder.CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::rmwOpId,            \
												   pointer,                                        \
												   value,                                          \
												   llvm::AtomicOrdering::SequentiallyConsistent);  \
		atomicRMW->setVolatile(true);                                                              \
		push(memToValue(atomicRMW, asLLVMType(llvmContext, ValueType::valueTypeId)));              \
	}

EMIT_ATOMIC_RMW(i32, atomic_rmw8_xchg_u, Xchg, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw16_xchg_u, Xchg, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw_xchg, Xchg, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_RMW(i64, atomic_rmw8_xchg_u, Xchg, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw16_xchg_u, Xchg, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw32_xchg_u, Xchg, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw_xchg, Xchg, llvmContext.i64Type, 3, identity, identity)

EMIT_ATOMIC_RMW(i32, atomic_rmw8_add_u, Add, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw16_add_u, Add, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw_add, Add, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_RMW(i64, atomic_rmw8_add_u, Add, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw16_add_u, Add, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw32_add_u, Add, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw_add, Add, llvmContext.i64Type, 3, identity, identity)

EMIT_ATOMIC_RMW(i32, atomic_rmw8_sub_u, Sub, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw16_sub_u, Sub, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw_sub, Sub, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_RMW(i64, atomic_rmw8_sub_u, Sub, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw16_sub_u, Sub, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw32_sub_u, Sub, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw_sub, Sub, llvmContext.i64Type, 3, identity, identity)

EMIT_ATOMIC_RMW(i32, atomic_rmw8_and_u, And, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw16_and_u, And, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw_and, And, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_RMW(i64, atomic_rmw8_and_u, And, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw16_and_u, And, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw32_and_u, And, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw_and, And, llvmContext.i64Type, 3, identity, identity)

EMIT_ATOMIC_RMW(i32, atomic_rmw8_or_u, Or, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw16_or_u, Or, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw_or, Or, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_RMW(i64, atomic_rmw8_or_u, Or, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw16_or_u, Or, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw32_or_u, Or, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw_or, Or, llvmContext.i64Type, 3, identity, identity)

EMIT_ATOMIC_RMW(i32, atomic_rmw8_xor_u, Xor, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw16_xor_u, Xor, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw_xor, Xor, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_RMW(i64, atomic_rmw8_xor_u, Xor, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw16_xor_u, Xor, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw32_xor_u, Xor, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw_xor, Xor, llvmContext.i64Type, 3, identity, identity)
