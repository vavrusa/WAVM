#include <stddef.h>
#include <atomic>
#include <memory>
#include <vector>
#include "RuntimePrivate.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Timing.h"
#include "WAVM/Platform/Memory.h"
#include "WAVM/Platform/RWMutex.h"
#include "WAVM/Runtime/Runtime.h"
#include "WAVM/RuntimeABI/RuntimeABI.h"

using namespace WAVM;
using namespace WAVM::Runtime;

Runtime::Compartment::Compartment()
: GCObject(ObjectKind::compartment, this)
, unalignedRuntimeData(nullptr)
, tables(0, maxTables - 1)
, memories(0, maxMemories - 1)
// Use UINTPTR_MAX as an invalid ID for globals, exception types, and instances.
, globals(0, UINTPTR_MAX - 1)
, exceptionTypes(0, UINTPTR_MAX - 1)
, instances(0, UINTPTR_MAX - 1)
, contexts(0, maxContexts - 1)
, foreigns(0, UINTPTR_MAX - 1)
{
	runtimeData = (CompartmentRuntimeData*)Platform::allocateAlignedVirtualPages(
		wavmCompartmentReservedBytes >> Platform::getBytesPerPageLog2(),
		compartmentRuntimeDataAlignmentLog2,
		unalignedRuntimeData);

	WAVM_ERROR_UNLESS(Platform::commitVirtualPages(
		(U8*)runtimeData,
		offsetof(CompartmentRuntimeData, contexts) >> Platform::getBytesPerPageLog2()));

	runtimeData->compartment = this;
}

Runtime::Compartment::~Compartment()
{
	Platform::RWMutex::ExclusiveLock compartmentLock(mutex);

	WAVM_ASSERT(!memories.size());
	WAVM_ASSERT(!tables.size());
	WAVM_ASSERT(!exceptionTypes.size());
	WAVM_ASSERT(!globals.size());
	WAVM_ASSERT(!instances.size());
	WAVM_ASSERT(!contexts.size());
	WAVM_ASSERT(!foreigns.size());

	Platform::freeAlignedVirtualPages(
		unalignedRuntimeData,
		wavmCompartmentReservedBytes >> Platform::getBytesPerPageLog2(),
		compartmentRuntimeDataAlignmentLog2);
	runtimeData = nullptr;
	unalignedRuntimeData = nullptr;
}

Compartment* Runtime::createCompartment() { return new Compartment; }

Compartment* Runtime::cloneCompartment(const Compartment* compartment)
{
	Timing::Timer timer;

	Compartment* newCompartment = new Compartment;
	Platform::RWMutex::ShareableLock compartmentLock(compartment->mutex);

	// Clone tables.
	for(Table* table : compartment->tables)
	{
		Table* newTable = cloneTable(table, newCompartment);
		WAVM_ASSERT(newTable->id == table->id);
	}

	// Clone memories.
	for(Memory* memory : compartment->memories)
	{
		Memory* newMemory = cloneMemory(memory, newCompartment);
		WAVM_ASSERT(newMemory->id == memory->id);
	}

	// Clone globals.
	newCompartment->globalDataAllocationMask = compartment->globalDataAllocationMask;
	memcpy(newCompartment->initialContextMutableGlobals,
		   compartment->initialContextMutableGlobals,
		   sizeof(newCompartment->initialContextMutableGlobals));
	for(Global* global : compartment->globals)
	{
		Global* newGlobal = cloneGlobal(global, newCompartment);
		WAVM_ASSERT(newGlobal->id == global->id);
		WAVM_ASSERT(newGlobal->mutableGlobalIndex == global->mutableGlobalIndex);
	}

	// Clone exception types.
	for(ExceptionType* exceptionType : compartment->exceptionTypes)
	{
		ExceptionType* newExceptionType = cloneExceptionType(exceptionType, newCompartment);
		WAVM_ASSERT(newExceptionType->id == exceptionType->id);
	}

	// Clone instances.
	for(Instance* instance : compartment->instances)
	{
		Instance* newInstance = cloneInstance(instance, newCompartment);
		WAVM_ASSERT(newInstance->id == instance->id);
	}

	Timing::logTimer("Cloned compartment", timer);
	return newCompartment;
}

Object* Runtime::remapToClonedCompartment(const Object* object, const Compartment* newCompartment)
{
	if(!object) { return nullptr; }
	if(object->kind == ObjectKind::function) { return const_cast<Object*>(object); }

	Platform::RWMutex::ShareableLock compartmentLock(newCompartment->mutex);
	switch(object->kind)
	{
	case ObjectKind::table: return newCompartment->tables[asTable(object)->id];
	case ObjectKind::memory: return newCompartment->memories[asMemory(object)->id];
	case ObjectKind::global: return newCompartment->globals[asGlobal(object)->id];
	case ObjectKind::exceptionType:
		return newCompartment->exceptionTypes[asExceptionType(object)->id];
	case ObjectKind::instance: return newCompartment->instances[asInstance(object)->id];

	case ObjectKind::function:
	case ObjectKind::context:
	case ObjectKind::compartment:
	case ObjectKind::foreign:
	case ObjectKind::invalid:
	default: WAVM_UNREACHABLE();
	};
}

Function* Runtime::remapToClonedCompartment(const Function* function,
											const Compartment* newCompartment)
{
	return const_cast<Function*>(function);
}
Table* Runtime::remapToClonedCompartment(const Table* table, const Compartment* newCompartment)
{
	if(!table) { return nullptr; }
	Platform::RWMutex::ShareableLock compartmentLock(newCompartment->mutex);
	return newCompartment->tables[table->id];
}
Memory* Runtime::remapToClonedCompartment(const Memory* memory, const Compartment* newCompartment)
{
	if(!memory) { return nullptr; }
	Platform::RWMutex::ShareableLock compartmentLock(newCompartment->mutex);
	return newCompartment->memories[memory->id];
}
Global* Runtime::remapToClonedCompartment(const Global* global, const Compartment* newCompartment)
{
	if(!global) { return nullptr; }
	Platform::RWMutex::ShareableLock compartmentLock(newCompartment->mutex);
	return newCompartment->globals[global->id];
}
ExceptionType* Runtime::remapToClonedCompartment(const ExceptionType* exceptionType,
												 const Compartment* newCompartment)
{
	if(!exceptionType) { return nullptr; }
	Platform::RWMutex::ShareableLock compartmentLock(newCompartment->mutex);
	return newCompartment->exceptionTypes[exceptionType->id];
}
Instance* Runtime::remapToClonedCompartment(const Instance* instance,
											const Compartment* newCompartment)
{
	if(!instance) { return nullptr; }
	Platform::RWMutex::ShareableLock compartmentLock(newCompartment->mutex);
	return newCompartment->instances[instance->id];
}
Foreign* Runtime::remapToClonedCompartment(const Foreign* foreign,
										   const Compartment* newCompartment)
{
	if(!foreign) { return nullptr; }
	Platform::RWMutex::ShareableLock compartmentLock(newCompartment->mutex);
	return newCompartment->foreigns[foreign->id];
}

bool Runtime::isInCompartment(const Object* object, const Compartment* compartment)
{
	if(object->kind == ObjectKind::function)
	{
		// The function may be in multiple compartments, but if this compartment maps the function's
		// instanceId to a Instance with the LLVMJIT LoadedModule that contains this
		// function, then the function is in this compartment.
		Function* function = (Function*)object;

		// Treat functions with instanceId=UINTPTR_MAX as if they are in all compartments.
		if(function->instanceId == UINTPTR_MAX) { return true; }

		Platform::RWMutex::ShareableLock compartmentLock(compartment->mutex);
		if(!compartment->instances.contains(function->instanceId)) { return false; }
		Instance* instance = compartment->instances[function->instanceId];
		return instance->jitModule.get() == function->mutableData->jitModule;
	}
	else
	{
		GCObject* gcObject = (GCObject*)object;
		return gcObject->compartment == compartment;
	}
}
