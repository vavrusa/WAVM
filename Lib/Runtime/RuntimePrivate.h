#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include "WAVM/IR/Module.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/DenseStaticIntSet.h"
#include "WAVM/Inline/HashMap.h"
#include "WAVM/Inline/HashSet.h"
#include "WAVM/Inline/IndexMap.h"
#include "WAVM/LLVMJIT/LLVMJIT.h"
#include "WAVM/Platform/Defines.h"
#include "WAVM/Platform/RWMutex.h"
#include "WAVM/Runtime/Intrinsics.h"
#include "WAVM/Runtime/Runtime.h"
#include "WAVM/RuntimeABI/RuntimeABI.h"

namespace WAVM { namespace Intrinsics {
	struct Module;
}}

namespace WAVM { namespace Runtime {

	// A private base class for all runtime objects that are garbage collected.
	struct GCObject : Object
	{
		Compartment* const compartment;
		mutable std::atomic<Uptr> numRootReferences{0};
		void* userData{nullptr};
		void (*finalizeUserData)(void*);

		GCObject(ObjectKind inKind, Compartment* inCompartment);
		virtual ~GCObject();
	};

	// An instance of a WebAssembly Table.
	struct Table : GCObject
	{
		struct Element
		{
			std::atomic<Uptr> biasedValue;
		};

		Uptr id = UINTPTR_MAX;
		const IR::TableType type;
		std::string debugName;

		Element* elements = nullptr;
		Uptr numReservedBytes = 0;
		Uptr numReservedElements = 0;

		mutable Platform::RWMutex resizingMutex;
		std::atomic<Uptr> numElements{0};

		ResourceQuotaRef resourceQuota;

		Table(Compartment* inCompartment,
			  const IR::TableType& inType,
			  std::string&& inDebugName,
			  ResourceQuotaRefParam inResourceQuota)
		: GCObject(ObjectKind::table, inCompartment)
		, type(inType)
		, debugName(std::move(inDebugName))
		, resourceQuota(inResourceQuota)
		{
		}
		~Table() override;
	};

	// This is used as a sentinel value for table elements that are out-of-bounds. The address of
	// this Object is subtracted from every address stored in the table, so zero-initialized pages
	// at the end of the array will, when re-adding this Function's address, point to this Object.
	extern Object* getOutOfBoundsElement();

	// An instance of a WebAssembly Memory.
	struct Memory : GCObject
	{
		Uptr id = UINTPTR_MAX;
		IR::MemoryType type;
		std::string debugName;

		U8* baseAddress = nullptr;
		Uptr numReservedBytes = 0;

		mutable Platform::RWMutex resizingMutex;
		std::atomic<Uptr> numPages{0};

		ResourceQuotaRef resourceQuota;

		Memory(Compartment* inCompartment,
			   const IR::MemoryType& inType,
			   std::string&& inDebugName,
			   ResourceQuotaRefParam inResourceQuota)
		: GCObject(ObjectKind::memory, inCompartment)
		, type(inType)
		, debugName(std::move(inDebugName))
		, resourceQuota(inResourceQuota)
		{
		}
		~Memory() override;
	};

	// An instance of a WebAssembly global.
	struct Global : GCObject
	{
		Uptr id = UINTPTR_MAX;

		const IR::GlobalType type;
		std::string debugName;
		const U32 mutableGlobalIndex;
		IR::UntaggedValue initialValue;
		bool hasBeenInitialized;

		Global(Compartment* inCompartment,
			   IR::GlobalType inType,
			   U32 inMutableGlobalId,
			   std::string&& inDebugName,
			   IR::UntaggedValue inInitialValue = IR::UntaggedValue())
		: GCObject(ObjectKind::global, inCompartment)
		, type(inType)
		, debugName(std::move(inDebugName))
		, mutableGlobalIndex(inMutableGlobalId)
		, initialValue(inInitialValue)
		, hasBeenInitialized(false)
		{
		}
		~Global() override;
	};

	// An instance of a WebAssembly exception type.
	struct ExceptionType : GCObject
	{
		Uptr id = UINTPTR_MAX;

		IR::ExceptionType sig;
		std::string debugName;

		ExceptionType(Compartment* inCompartment,
					  IR::ExceptionType inSig,
					  std::string&& inDebugName)
		: GCObject(ObjectKind::exceptionType, inCompartment)
		, sig(inSig)
		, debugName(std::move(inDebugName))
		{
		}

		~ExceptionType() override;
	};

	typedef std::vector<std::shared_ptr<std::vector<U8>>> DataSegmentVector;
	typedef std::vector<std::shared_ptr<IR::ElemSegment::Contents>> ElemSegmentVector;

	// A compiled WebAssembly module.
	struct Module
	{
		IR::Module ir;
		std::vector<U8> objectCode;

		Module(IR::Module&& inIR, std::vector<U8>&& inObjectCode)
		: ir(inIR), objectCode(std::move(inObjectCode))
		{
		}
	};

	// An instance of a WebAssembly module.
	struct Instance : GCObject
	{
		const Uptr id;
		const std::string debugName;

		const HashMap<std::string, Object*> exportMap;
		const std::vector<Object*> exports;

		const std::vector<Function*> functions;
		const std::vector<Table*> tables;
		const std::vector<Memory*> memories;
		const std::vector<Global*> globals;
		const std::vector<ExceptionType*> exceptionTypes;

		Function* const startFunction;

		mutable Platform::RWMutex dataSegmentsMutex;
		DataSegmentVector dataSegments;

		mutable Platform::RWMutex elemSegmentsMutex;
		ElemSegmentVector elemSegments;

		const std::shared_ptr<LLVMJIT::Module> jitModule;

		ResourceQuotaRef resourceQuota;

		Instance(Compartment* inCompartment,
				 Uptr inID,
				 HashMap<std::string, Object*>&& inExportMap,
				 std::vector<Object*>&& inExports,
				 std::vector<Function*>&& inFunctions,
				 std::vector<Table*>&& inTables,
				 std::vector<Memory*>&& inMemories,
				 std::vector<Global*>&& inGlobals,
				 std::vector<ExceptionType*>&& inExceptionTypes,
				 Function* inStartFunction,
				 DataSegmentVector&& inPassiveDataSegments,
				 ElemSegmentVector&& inPassiveElemSegments,
				 std::shared_ptr<LLVMJIT::Module>&& inJITModule,
				 std::string&& inDebugName,
				 ResourceQuotaRefParam inResourceQuota)
		: GCObject(ObjectKind::instance, inCompartment)
		, id(inID)
		, debugName(std::move(inDebugName))
		, exportMap(std::move(inExportMap))
		, exports(std::move(inExports))
		, functions(std::move(inFunctions))
		, tables(std::move(inTables))
		, memories(std::move(inMemories))
		, globals(std::move(inGlobals))
		, exceptionTypes(std::move(inExceptionTypes))
		, startFunction(inStartFunction)
		, dataSegments(std::move(inPassiveDataSegments))
		, elemSegments(std::move(inPassiveElemSegments))
		, jitModule(std::move(inJITModule))
		, resourceQuota(inResourceQuota)
		{
		}

		virtual ~Instance() override;
	};

	struct Context : GCObject
	{
		Uptr id = UINTPTR_MAX;
		struct ContextRuntimeData* runtimeData = nullptr;

		Context(Compartment* inCompartment) : GCObject(ObjectKind::context, inCompartment) {}
		~Context();
	};

	struct Compartment : GCObject
	{
		mutable Platform::RWMutex mutex;

		struct CompartmentRuntimeData* runtimeData;
		U8* unalignedRuntimeData;

		IndexMap<Uptr, Table*> tables;
		IndexMap<Uptr, Memory*> memories;
		IndexMap<Uptr, Global*> globals;
		IndexMap<Uptr, ExceptionType*> exceptionTypes;
		IndexMap<Uptr, Instance*> instances;
		IndexMap<Uptr, Context*> contexts;
		IndexMap<Uptr, Foreign*> foreigns;

		DenseStaticIntSet<U32, maxMutableGlobals> globalDataAllocationMask;
		IR::UntaggedValue initialContextMutableGlobals[maxMutableGlobals];

		Compartment();
		~Compartment();
	};

	struct Foreign : GCObject
	{
		Uptr id = UINTPTR_MAX;

		Foreign(Compartment* inCompartment) : GCObject(ObjectKind::foreign, inCompartment) {}
	};

	struct ResourceQuota
	{
		template<typename Value> struct CurrentAndMax
		{
			CurrentAndMax(Value inMax) : current{0}, max(inMax) {}

			bool allocate(Value delta)
			{
				Platform::RWMutex::ExclusiveLock lock(mutex);

				// Make sure the delta doesn't make current overflow.
				if(current + delta < current) { return false; }

				if(current + delta > max) { return false; }

				current += delta;
				return true;
			}

			void free(Value delta)
			{
				Platform::RWMutex::ExclusiveLock lock(mutex);
				WAVM_ASSERT(current - delta <= current);
				current -= delta;
			}

			Value getCurrent() const
			{
				Platform::RWMutex::ShareableLock lock(mutex);
				return current;
			}
			Value getMax() const
			{
				Platform::RWMutex::ShareableLock lock(mutex);
				return max;
			}
			void setMax(Value newMax)
			{
				Platform::RWMutex::ExclusiveLock lock(mutex);
				max = newMax;
			}

		private:
			mutable Platform::RWMutex mutex;
			Value current;
			Value max;
		};

		CurrentAndMax<Uptr> memoryPages{UINTPTR_MAX};
		CurrentAndMax<Uptr> tableElems{UINTPTR_MAX};
	};

	WAVM_DECLARE_INTRINSIC_MODULE(wavmIntrinsics);
	WAVM_DECLARE_INTRINSIC_MODULE(wavmIntrinsicsAtomics);
	WAVM_DECLARE_INTRINSIC_MODULE(wavmIntrinsicsException);
	WAVM_DECLARE_INTRINSIC_MODULE(wavmIntrinsicsMemory);
	WAVM_DECLARE_INTRINSIC_MODULE(wavmIntrinsicsTable);

	// Checks whether an address is owned by a table or memory.
	bool isAddressOwnedByTable(U8* address, Table*& outTable, Uptr& outTableIndex);
	bool isAddressOwnedByMemory(U8* address, Memory*& outMemory, Uptr& outMemoryAddress);

	// Clones objects into a new compartment with the same ID.
	Table* cloneTable(Table* memory, Compartment* newCompartment);
	Memory* cloneMemory(Memory* memory, Compartment* newCompartment);
	ExceptionType* cloneExceptionType(ExceptionType* exceptionType, Compartment* newCompartment);
	Instance* cloneInstance(Instance* instance, Compartment* newCompartment);

	// Clone a global with same ID and mutable data offset (if mutable) in a new compartment.
	Global* cloneGlobal(Global* global, Compartment* newCompartment);

	Instance* getInstanceFromRuntimeData(ContextRuntimeData* contextRuntimeData, Uptr instanceId);
	Table* getTableFromRuntimeData(ContextRuntimeData* contextRuntimeData, Uptr tableId);
	Memory* getMemoryFromRuntimeData(ContextRuntimeData* contextRuntimeData, Uptr memoryId);

	// Initialize a data segment (equivalent to executing a memory.init instruction).
	void initDataSegment(Instance* instance,
						 Uptr dataSegmentIndex,
						 const std::vector<U8>* dataVector,
						 Memory* memory,
						 Uptr destAddress,
						 Uptr sourceOffset,
						 Uptr numBytes);

	// Initialize a table segment (equivalent to executing a table.init instruction).
	void initElemSegment(Instance* instance,
						 Uptr elemSegmentIndex,
						 const IR::ElemSegment::Contents* contents,
						 Table* table,
						 Uptr destOffset,
						 Uptr sourceOffset,
						 Uptr numElems);

	// This function is like Runtime::instantiateModule, but allows binding function imports
	// directly to native code. The interpretation of each FunctionImportBinding is determined by
	// the import's calling convention:
	// An import with CallingConvention::wasm reads FunctionImportBinding::wasmFunction,
	// but all other imports read FunctionImportBinding::nativeFunction.
	struct FunctionImportBinding
	{
		union
		{
			Function* wasmFunction;
			const void* nativeFunction;
		};

		FunctionImportBinding(Function* inWASMFunction) : wasmFunction(inWASMFunction) {}
		FunctionImportBinding(void* inNativeFunction) : nativeFunction(inNativeFunction) {}
	};
	Instance* instantiateModuleInternal(Compartment* compartment,
										ModuleConstRefParam module,
										std::vector<FunctionImportBinding>&& functionImports,
										std::vector<Table*>&& tableImports,
										std::vector<Memory*>&& memoryImports,
										std::vector<Global*>&& globalImports,
										std::vector<ExceptionType*>&& exceptionTypeImports,
										std::string&& debugName,
										ResourceQuotaRefParam resourceQuota = ResourceQuotaRef());
}}

namespace WAVM { namespace Intrinsics {
	HashMap<std::string, Function*> getUninstantiatedFunctions(
		const std::initializer_list<const Intrinsics::Module*>& moduleRefs);
}}
