// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/code_generator.h"

#include "vm/assembler.h"
#include "vm/ast.h"
#include "vm/bigint_operations.h"
#include "vm/code_patcher.h"
#include "vm/compiler.h"
#include "vm/dart_api_impl.h"
#include "vm/dart_entry.h"
#include "vm/debugger.h"
#include "vm/deopt_instructions.h"
#include "vm/exceptions.h"
#include "vm/intermediate_language.h"
#include "vm/object_store.h"
#include "vm/message.h"
#include "vm/message_handler.h"
#include "vm/parser.h"
#include "vm/resolver.h"
#include "vm/runtime_entry.h"
#include "vm/stack_frame.h"
#include "vm/symbols.h"
#include "vm/verifier.h"

namespace dart {

DEFINE_FLAG(bool, deoptimize_alot, false,
    "Deoptimizes all live frames when we are about to return to Dart code from"
    " native entries.");
DEFINE_FLAG(int, max_subtype_cache_entries, 100,
    "Maximum number of subtype cache entries (number of checks cached).");
DEFINE_FLAG(int, optimization_counter_threshold, 15000,
    "Function's usage-counter value before it is optimized, -1 means never");
DEFINE_FLAG(charp, optimization_filter, NULL, "Optimize only named function");
DEFINE_FLAG(int, reoptimization_counter_threshold, 2000,
    "Counter threshold before a function gets reoptimized.");
DEFINE_FLAG(bool, stop_on_excessive_deoptimization, false,
    "Debugging: stops program if deoptimizing same function too often");
DEFINE_FLAG(bool, trace_deoptimization, false, "Trace deoptimization");
DEFINE_FLAG(bool, trace_deoptimization_verbose, false,
    "Trace deoptimization verbose");
DEFINE_FLAG(bool, trace_failed_optimization_attempts, false,
    "Traces all failed optimization attempts");
DEFINE_FLAG(bool, trace_ic, false, "Trace IC handling");
DEFINE_FLAG(bool, trace_ic_miss_in_optimized, false,
    "Trace IC miss in optimized code");
DEFINE_FLAG(bool, trace_optimized_ic_calls, false,
    "Trace IC calls in optimized code.");
DEFINE_FLAG(bool, trace_patching, false, "Trace patching of code.");
DEFINE_FLAG(bool, trace_runtime_calls, false, "Trace runtime calls");

DECLARE_FLAG(int, deoptimization_counter_threshold);
DECLARE_FLAG(bool, enable_type_checks);
DECLARE_FLAG(bool, report_usage_count);
DECLARE_FLAG(bool, trace_type_checks);

DEFINE_FLAG(bool, use_osr, true, "Use on-stack replacement.");
DEFINE_FLAG(bool, trace_osr, false, "Trace attempts at on-stack replacement.");


DEFINE_RUNTIME_ENTRY(TraceFunctionEntry, 1) {
  const Function& function = Function::CheckedHandle(arguments.ArgAt(0));
  const String& function_name = String::Handle(function.name());
  const String& class_name =
      String::Handle(Class::Handle(function.Owner()).Name());
  OS::PrintErr("> Entering '%s.%s'\n",
      class_name.ToCString(), function_name.ToCString());
}


DEFINE_RUNTIME_ENTRY(TraceFunctionExit, 1) {
  const Function& function = Function::CheckedHandle(arguments.ArgAt(0));
  const String& function_name = String::Handle(function.name());
  const String& class_name =
      String::Handle(Class::Handle(function.Owner()).Name());
  OS::PrintErr("< Exiting '%s.%s'\n",
      class_name.ToCString(), function_name.ToCString());
}


// Allocation of a fixed length array of given element type.
// This runtime entry is never called for allocating a List of a generic type,
// because a prior run time call instantiates the element type if necessary.
// Arg0: array length.
// Arg1: array type arguments, i.e. vector of 1 type, the element type.
// Return value: newly allocated array of length arg0.
DEFINE_RUNTIME_ENTRY(AllocateArray, 2) {
  const Smi& length = Smi::CheckedHandle(arguments.ArgAt(0));
  const Array& array = Array::Handle(Array::New(length.Value()));
  arguments.SetReturn(array);
  AbstractTypeArguments& element_type =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(1));
  // An Array is raw or takes one type argument. However, its type argument
  // vector may be longer than 1 due to a type optimization reusing the type
  // argument vector of the instantiator.
  ASSERT(element_type.IsNull() ||
         ((element_type.Length() >= 1) && element_type.IsInstantiated()));
  array.SetTypeArguments(element_type);  // May be null.
}


// Allocate a new object.
// Arg0: class of the object that needs to be allocated.
// Arg1: type arguments of the object that needs to be allocated.
// Arg2: type arguments of the instantiator or kNoInstantiator.
// Return value: newly allocated object.
DEFINE_RUNTIME_ENTRY(AllocateObject, 3) {
  const Class& cls = Class::CheckedHandle(arguments.ArgAt(0));
  const Instance& instance = Instance::Handle(Instance::New(cls));
  arguments.SetReturn(instance);
  if (cls.NumTypeArguments() == 0) {
    // No type arguments required for a non-parameterized type.
    ASSERT(Instance::CheckedHandle(arguments.ArgAt(1)).IsNull());
    return;
  }
  AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(1));
  // If no instantiator is provided, set the type arguments and return.
  if (Object::Handle(arguments.ArgAt(2)).IsSmi()) {
    ASSERT(Smi::CheckedHandle(arguments.ArgAt(2)).Value() ==
           StubCode::kNoInstantiator);
    // Unless null (for a raw type), the type argument vector may be longer than
    // necessary due to a type optimization reusing the type argument vector of
    // the instantiator.
    ASSERT(type_arguments.IsNull() ||
           (type_arguments.IsInstantiated() &&
            (type_arguments.Length() >= cls.NumTypeArguments())));
    instance.SetTypeArguments(type_arguments);  // May be null.
    return;
  }
  // A still uninstantiated type argument vector must have the correct length.
  ASSERT(!type_arguments.IsInstantiated() &&
         (type_arguments.Length() == cls.NumTypeArguments()));
  const AbstractTypeArguments& instantiator =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(2));
  ASSERT(instantiator.IsNull() || instantiator.IsInstantiated());
  // Code inlined in the caller should have optimized the case where the
  // instantiator can be reused as type argument vector.
  ASSERT(instantiator.IsNull() || !type_arguments.IsUninstantiatedIdentity());
  type_arguments = InstantiatedTypeArguments::New(type_arguments, instantiator);
  instance.SetTypeArguments(type_arguments);
}


// Helper returning the token position of the Dart caller.
static intptr_t GetCallerLocation() {
  DartFrameIterator iterator;
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);
  return caller_frame->GetTokenPos();
}


// Allocate a new object of a generic type and check that the instantiated type
// arguments are within the declared bounds or throw a dynamic type error.
// Arg0: class of the object that needs to be allocated.
// Arg1: type arguments of the object that needs to be allocated.
// Arg2: type arguments of the instantiator or kNoInstantiator.
// Return value: newly allocated object.
DEFINE_RUNTIME_ENTRY(AllocateObjectWithBoundsCheck, 3) {
  ASSERT(FLAG_enable_type_checks);
  const Class& cls = Class::CheckedHandle(arguments.ArgAt(0));
  const Instance& instance = Instance::Handle(Instance::New(cls));
  arguments.SetReturn(instance);
  ASSERT(cls.NumTypeArguments() > 0);
  AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(1));
  if (Object::Handle(arguments.ArgAt(2)).IsSmi()) {
    ASSERT(Smi::CheckedHandle(arguments.ArgAt(2)).Value() ==
           StubCode::kNoInstantiator);
    // Unless null (for a raw type), the type argument vector may be longer than
    // necessary due to a type optimization reusing the type argument vector of
    // the instantiator.
    ASSERT(type_arguments.IsNull() ||
           (type_arguments.IsInstantiated() &&
            (type_arguments.Length() >= cls.NumTypeArguments())));
  } else {
    // A still uninstantiated type argument vector must have the correct length.
    ASSERT(!type_arguments.IsInstantiated() &&
           (type_arguments.Length() == cls.NumTypeArguments()));
    const AbstractTypeArguments& instantiator =
        AbstractTypeArguments::CheckedHandle(arguments.ArgAt(2));
    ASSERT(instantiator.IsNull() || instantiator.IsInstantiated());
    Error& bound_error = Error::Handle();
    // Code inlined in the caller should have optimized the case where the
    // instantiator can be reused as type argument vector.
  ASSERT(instantiator.IsNull() || !type_arguments.IsUninstantiatedIdentity());
    type_arguments = type_arguments.InstantiateFrom(instantiator, &bound_error);
    if (!bound_error.IsNull()) {
      // Throw a dynamic type error.
      const intptr_t location = GetCallerLocation();
      String& bound_error_message =  String::Handle(
          String::New(bound_error.ToErrorCString()));
      Exceptions::CreateAndThrowTypeError(
          location, Symbols::Empty(), Symbols::Empty(),
          Symbols::Empty(), bound_error_message);
      UNREACHABLE();
    }
  }
  ASSERT(type_arguments.IsNull() || type_arguments.IsInstantiated());
  instance.SetTypeArguments(type_arguments);
}


// Instantiate type.
// Arg0: uninstantiated type.
// Arg1: instantiator type arguments.
// Return value: instantiated type.
DEFINE_RUNTIME_ENTRY(InstantiateType, 2) {
  AbstractType& type = AbstractType::CheckedHandle(arguments.ArgAt(0));
  const AbstractTypeArguments& instantiator =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(1));
  ASSERT(!type.IsNull() && !type.IsInstantiated());
  ASSERT(instantiator.IsNull() || instantiator.IsInstantiated());
  Error& bound_error = Error::Handle();
  type = type.InstantiateFrom(instantiator, &bound_error);
  if (!bound_error.IsNull()) {
    // Throw a dynamic type error.
    const intptr_t location = GetCallerLocation();
    String& bound_error_message =  String::Handle(
        String::New(bound_error.ToErrorCString()));
    Exceptions::CreateAndThrowTypeError(
        location, Symbols::Empty(), Symbols::Empty(),
        Symbols::Empty(), bound_error_message);
    UNREACHABLE();
  }
  ASSERT(!type.IsNull() && type.IsInstantiated());
  arguments.SetReturn(type);
}


// Instantiate type arguments.
// Arg0: uninstantiated type arguments.
// Arg1: instantiator type arguments.
// Return value: instantiated type arguments.
DEFINE_RUNTIME_ENTRY(InstantiateTypeArguments, 2) {
  AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(0));
  const AbstractTypeArguments& instantiator =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(1));
  ASSERT(!type_arguments.IsNull() && !type_arguments.IsInstantiated());
  ASSERT(instantiator.IsNull() || instantiator.IsInstantiated());
  // Code inlined in the caller should have optimized the case where the
  // instantiator can be reused as type argument vector.
  ASSERT(instantiator.IsNull() || !type_arguments.IsUninstantiatedIdentity());
  type_arguments = InstantiatedTypeArguments::New(type_arguments, instantiator);
  ASSERT(type_arguments.IsInstantiated());
  arguments.SetReturn(type_arguments);
}


// Allocate a new closure.
// The type argument vector of a closure is always the vector of type parameters
// of its signature class, i.e. an uninstantiated identity vector. Therefore,
// the instantiator type arguments can be used as the instantiated closure type
// arguments and is passed here as the type arguments.
// Arg0: local function.
// Arg1: type arguments of the closure (i.e. instantiator).
// Return value: newly allocated closure.
DEFINE_RUNTIME_ENTRY(AllocateClosure, 2) {
  const Function& function = Function::CheckedHandle(arguments.ArgAt(0));
  ASSERT(function.IsClosureFunction() && !function.IsImplicitClosureFunction());
  const AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(1));
  ASSERT(type_arguments.IsNull() || type_arguments.IsInstantiated());
  // The current context was saved in the Isolate structure when entering the
  // runtime.
  const Context& context = Context::Handle(isolate->top_context());
  ASSERT(!context.IsNull());
  const Instance& closure = Instance::Handle(Closure::New(function, context));
  Closure::SetTypeArguments(closure, type_arguments);
  arguments.SetReturn(closure);
}


// Allocate a new implicit instance closure.
// Arg0: local function.
// Arg1: receiver object.
// Arg2: type arguments of the closure.
// Return value: newly allocated closure.
DEFINE_RUNTIME_ENTRY(AllocateImplicitInstanceClosure, 3) {
  const Function& function = Function::CheckedHandle(arguments.ArgAt(0));
  ASSERT(function.IsImplicitInstanceClosureFunction());
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(1));
  const AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(2));
  ASSERT(type_arguments.IsNull() || type_arguments.IsInstantiated());
  Context& context = Context::Handle();
  context = Context::New(1);
  context.SetAt(0, receiver);
  const Instance& closure = Instance::Handle(Closure::New(function, context));
  Closure::SetTypeArguments(closure, type_arguments);
  arguments.SetReturn(closure);
}


// Allocate a new context large enough to hold the given number of variables.
// Arg0: number of variables.
// Return value: newly allocated context.
DEFINE_RUNTIME_ENTRY(AllocateContext, 1) {
  const Smi& num_variables = Smi::CheckedHandle(arguments.ArgAt(0));
  arguments.SetReturn(Context::Handle(Context::New(num_variables.Value())));
}


// Make a copy of the given context, including the values of the captured
// variables.
// Arg0: the context to be cloned.
// Return value: newly allocated context.
DEFINE_RUNTIME_ENTRY(CloneContext, 1) {
  const Context& ctx = Context::CheckedHandle(arguments.ArgAt(0));
  Context& cloned_ctx = Context::Handle(Context::New(ctx.num_variables()));
  cloned_ctx.set_parent(Context::Handle(ctx.parent()));
  for (int i = 0; i < ctx.num_variables(); i++) {
    cloned_ctx.SetAt(i, Instance::Handle(ctx.At(i)));
  }
  arguments.SetReturn(cloned_ctx);
}


// Helper routine for tracing a type check.
static void PrintTypeCheck(
    const char* message,
    const Instance& instance,
    const AbstractType& type,
    const AbstractTypeArguments& instantiator_type_arguments,
    const Bool& result) {
  DartFrameIterator iterator;
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);

  const Type& instance_type = Type::Handle(instance.GetType());
  ASSERT(instance_type.IsInstantiated());
  if (type.IsInstantiated()) {
    OS::PrintErr("%s: '%s' %" Pd " %s '%s' %" Pd " (pc: %#" Px ").\n",
                 message,
                 String::Handle(instance_type.Name()).ToCString(),
                 Class::Handle(instance_type.type_class()).id(),
                 (result.raw() == Bool::True().raw()) ? "is" : "is !",
                 String::Handle(type.Name()).ToCString(),
                 Class::Handle(type.type_class()).id(),
                 caller_frame->pc());
  } else {
    // Instantiate type before printing.
    Error& bound_error = Error::Handle();
    const AbstractType& instantiated_type = AbstractType::Handle(
        type.InstantiateFrom(instantiator_type_arguments, &bound_error));
    OS::PrintErr("%s: '%s' %s '%s' instantiated from '%s' (pc: %#" Px ").\n",
                 message,
                 String::Handle(instance_type.Name()).ToCString(),
                 (result.raw() == Bool::True().raw()) ? "is" : "is !",
                 String::Handle(instantiated_type.Name()).ToCString(),
                 String::Handle(type.Name()).ToCString(),
                 caller_frame->pc());
    if (!bound_error.IsNull()) {
      OS::Print("  bound error: %s\n", bound_error.ToErrorCString());
    }
  }
  const Function& function = Function::Handle(
      caller_frame->LookupDartFunction());
  OS::PrintErr(" -> Function %s\n", function.ToFullyQualifiedCString());
}


// Converts InstantiatedTypeArguments to TypeArguments and stores it
// into the instance. The assembly code can handle only type arguments of
// class TypeArguments. Because of the overhead, do it only when needed.
// Return true if type arguments have been replaced, false otherwise.
static bool OptimizeTypeArguments(const Instance& instance) {
  const Class& type_class = Class::ZoneHandle(instance.clazz());
  if (type_class.NumTypeArguments() == 0) {
    return false;
  }
  AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::Handle(instance.GetTypeArguments());
  if (type_arguments.IsNull()) {
    return false;
  }
  bool replaced = false;
  if (type_arguments.IsInstantiatedTypeArguments()) {
    AbstractTypeArguments& uninstantiated = AbstractTypeArguments::Handle();
    AbstractTypeArguments& instantiator = AbstractTypeArguments::Handle();
    do {
      const InstantiatedTypeArguments& instantiated_type_arguments =
          InstantiatedTypeArguments::Cast(type_arguments);
      uninstantiated =
          instantiated_type_arguments.uninstantiated_type_arguments();
      instantiator = instantiated_type_arguments.instantiator_type_arguments();
      Error& bound_error = Error::Handle();
      type_arguments = uninstantiated.InstantiateFrom(instantiator,
                                                      &bound_error);
      ASSERT(bound_error.IsNull());  // Malbounded types are not optimized.
    } while (type_arguments.IsInstantiatedTypeArguments());
    AbstractTypeArguments& new_type_arguments = AbstractTypeArguments::Handle();
    new_type_arguments = type_arguments.Canonicalize();
    instance.SetTypeArguments(new_type_arguments);
    replaced = true;
  } else if (!type_arguments.IsCanonical()) {
    AbstractTypeArguments& new_type_arguments = AbstractTypeArguments::Handle();
    new_type_arguments = type_arguments.Canonicalize();
    instance.SetTypeArguments(new_type_arguments);
    replaced = true;
  }
  ASSERT(AbstractTypeArguments::Handle(
      instance.GetTypeArguments()).IsTypeArguments());
  return replaced;
}


// This updates the type test cache, an array containing 4-value elements
// (instance class, instance type arguments, instantiator type arguments and
// test_result). It can be applied to classes with type arguments in which
// case it contains just the result of the class subtype test, not including
// the evaluation of type arguments.
// This operation is currently very slow (lookup of code is not efficient yet).
// 'instantiator' can be null, in which case inst_targ
static void UpdateTypeTestCache(
    const Instance& instance,
    const AbstractType& type,
    const Instance& instantiator,
    const AbstractTypeArguments& incoming_instantiator_type_arguments,
    const Bool& result,
    const SubtypeTestCache& new_cache) {
  // Since the test is expensive, don't do it unless necessary.
  // The list of disallowed cases will decrease as they are implemented in
  // inlined assembly.
  if (new_cache.IsNull()) return;
  // Instantiator type arguments may be canonicalized later.
  AbstractTypeArguments& instantiator_type_arguments =
      AbstractTypeArguments::Handle(incoming_instantiator_type_arguments.raw());
  AbstractTypeArguments& instance_type_arguments =
      AbstractTypeArguments::Handle();
  const Class& instance_class = Class::Handle(instance.clazz());

  // Canonicalize type arguments.
  bool type_arguments_replaced = false;
  if (instance_class.NumTypeArguments() > 0) {
    // Canonicalize type arguments.
    type_arguments_replaced = OptimizeTypeArguments(instance);
    instance_type_arguments = instance.GetTypeArguments();
  }
  if (!instantiator.IsNull()) {
    if (OptimizeTypeArguments(instantiator)) {
      type_arguments_replaced = true;
    }
    instantiator_type_arguments = instantiator.GetTypeArguments();
  }

  intptr_t last_instance_class_id = -1;
  AbstractTypeArguments& last_instance_type_arguments =
      AbstractTypeArguments::Handle();
  AbstractTypeArguments& last_instantiator_type_arguments =
      AbstractTypeArguments::Handle();
  Bool& last_result = Bool::Handle();
  const intptr_t len = new_cache.NumberOfChecks();
  if (len >= FLAG_max_subtype_cache_entries) {
    return;
  }
  for (intptr_t i = 0; i < len; ++i) {
    new_cache.GetCheck(
        i,
        &last_instance_class_id,
        &last_instance_type_arguments,
        &last_instantiator_type_arguments,
        &last_result);
    if ((last_instance_class_id == instance_class.id()) &&
        (last_instance_type_arguments.raw() == instance_type_arguments.raw()) &&
        (last_instantiator_type_arguments.raw() ==
         instantiator_type_arguments.raw())) {
      if (FLAG_trace_type_checks) {
        OS::PrintErr("%" Pd " ", i);
        if (type_arguments_replaced) {
          PrintTypeCheck("Duplicate cache entry (canonical.)", instance, type,
              instantiator_type_arguments, result);
        } else {
          PrintTypeCheck("WARNING Duplicate cache entry", instance, type,
              instantiator_type_arguments, result);
        }
      }
      // Can occur if we have canonicalized arguments.
      // TODO(srdjan): Investigate why this assert can fail.
      // ASSERT(type_arguments_replaced);
      return;
    }
  }
  if (!instantiator_type_arguments.IsInstantiatedTypeArguments()) {
    new_cache.AddCheck(instance_class.id(),
                       instance_type_arguments,
                       instantiator_type_arguments,
                       result);
  }
  if (FLAG_trace_type_checks) {
    AbstractType& test_type = AbstractType::Handle(type.raw());
    if (!test_type.IsInstantiated()) {
      Error& bound_error = Error::Handle();
      test_type = type.InstantiateFrom(instantiator_type_arguments,
                                       &bound_error);
      ASSERT(bound_error.IsNull());  // Malbounded types are not optimized.
    }
    OS::PrintErr("  Updated test cache %p ix: %" Pd " with "
        "(cid: %" Pd ", type-args: %p, instantiator: %p, result: %s)\n"
        "    instance  [class: (%p '%s' cid: %" Pd "),    type-args: %p %s]\n"
        "    test-type [class: (%p '%s' cid: %" Pd "), in-type-args: %p %s]\n",
        new_cache.raw(),
        len,

        instance_class.id(),
        instance_type_arguments.raw(),
        instantiator_type_arguments.raw(),
        result.ToCString(),

        instance_class.raw(),
        String::Handle(instance_class.Name()).ToCString(),
        instance_class.id(),
        instance_type_arguments.raw(),
        instance_type_arguments.ToCString(),

        test_type.type_class(),
        String::Handle(Class::Handle(test_type.type_class()).Name()).
            ToCString(),
        Class::Handle(test_type.type_class()).id(),
        instantiator_type_arguments.raw(),
        instantiator_type_arguments.ToCString());
  }
}


// Check that the given instance is an instance of the given type.
// Tested instance may not be null, because the null test is inlined.
// Arg0: instance being checked.
// Arg1: type.
// Arg2: instantiator (or null).
// Arg3: type arguments of the instantiator of the type.
// Arg4: SubtypeTestCache.
// Return value: true or false, or may throw a type error in checked mode.
DEFINE_RUNTIME_ENTRY(Instanceof, 5) {
  const Instance& instance = Instance::CheckedHandle(arguments.ArgAt(0));
  const AbstractType& type = AbstractType::CheckedHandle(arguments.ArgAt(1));
  const Instance& instantiator = Instance::CheckedHandle(arguments.ArgAt(2));
  const AbstractTypeArguments& instantiator_type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(3));
  const SubtypeTestCache& cache =
      SubtypeTestCache::CheckedHandle(arguments.ArgAt(4));
  ASSERT(type.IsFinalized());
  ASSERT(!type.IsDynamicType());  // No need to check assignment.
  ASSERT(!type.IsMalformed());  // Already checked in code generator.
  ASSERT(!type.IsMalbounded());  // Already checked in code generator.
  Error& bound_error = Error::Handle();
  const Bool& result =
      Bool::Get(instance.IsInstanceOf(type,
                                      instantiator_type_arguments,
                                      &bound_error));
  if (FLAG_trace_type_checks) {
    PrintTypeCheck("InstanceOf",
        instance, type, instantiator_type_arguments, result);
  }
  if (!result.value() && !bound_error.IsNull()) {
    // Throw a dynamic type error only if the instanceof test fails.
    const intptr_t location = GetCallerLocation();
    String& bound_error_message =  String::Handle(
        String::New(bound_error.ToErrorCString()));
    Exceptions::CreateAndThrowTypeError(
        location, Symbols::Empty(), Symbols::Empty(),
        Symbols::Empty(), bound_error_message);
    UNREACHABLE();
  }
  UpdateTypeTestCache(instance, type, instantiator,
                      instantiator_type_arguments, result, cache);
  arguments.SetReturn(result);
}


// Check that the type of the given instance is a subtype of the given type and
// can therefore be assigned.
// Arg0: instance being assigned.
// Arg1: type being assigned to.
// Arg2: instantiator (or null).
// Arg3: type arguments of the instantiator of the type being assigned to.
// Arg4: name of variable being assigned to.
// Arg5: SubtypeTestCache.
// Return value: instance if a subtype, otherwise throw a TypeError.
DEFINE_RUNTIME_ENTRY(TypeCheck, 6) {
  const Instance& src_instance = Instance::CheckedHandle(arguments.ArgAt(0));
  const AbstractType& dst_type =
      AbstractType::CheckedHandle(arguments.ArgAt(1));
  const Instance& dst_instantiator =
      Instance::CheckedHandle(arguments.ArgAt(2));
  const AbstractTypeArguments& instantiator_type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(3));
  const String& dst_name = String::CheckedHandle(arguments.ArgAt(4));
  const SubtypeTestCache& cache =
      SubtypeTestCache::CheckedHandle(arguments.ArgAt(5));
  ASSERT(!dst_type.IsDynamicType());  // No need to check assignment.
  ASSERT(!dst_type.IsMalformed());  // Already checked in code generator.
  ASSERT(!dst_type.IsMalbounded());  // Already checked in code generator.
  ASSERT(!src_instance.IsNull());  // Already checked in inlined code.

  Error& bound_error = Error::Handle();
  const bool is_instance_of = src_instance.IsInstanceOf(
      dst_type, instantiator_type_arguments, &bound_error);

  if (FLAG_trace_type_checks) {
    PrintTypeCheck("TypeCheck",
                   src_instance, dst_type, instantiator_type_arguments,
                   Bool::Get(is_instance_of));
  }
  if (!is_instance_of) {
    // Throw a dynamic type error.
    const intptr_t location = GetCallerLocation();
    const AbstractType& src_type = AbstractType::Handle(src_instance.GetType());
    const String& src_type_name = String::Handle(src_type.UserVisibleName());
    String& dst_type_name = String::Handle();
    if (!dst_type.IsInstantiated()) {
      // Instantiate dst_type before reporting the error.
      const AbstractType& instantiated_dst_type = AbstractType::Handle(
          dst_type.InstantiateFrom(instantiator_type_arguments, NULL));
      // Note that instantiated_dst_type may be malbounded.
      dst_type_name = instantiated_dst_type.UserVisibleName();
    } else {
      dst_type_name = dst_type.UserVisibleName();
    }
    String& bound_error_message =  String::Handle();
    if (!bound_error.IsNull()) {
      ASSERT(FLAG_enable_type_checks);
      bound_error_message = String::New(bound_error.ToErrorCString());
    }
    Exceptions::CreateAndThrowTypeError(location, src_type_name, dst_type_name,
                                        dst_name, bound_error_message);
    UNREACHABLE();
  }
  UpdateTypeTestCache(src_instance, dst_type,
                      dst_instantiator, instantiator_type_arguments,
                      Bool::True(), cache);
  arguments.SetReturn(src_instance);
}


// Report that the type of the given object is not bool in conditional context.
// Arg0: bad object.
// Return value: none, throws a TypeError.
DEFINE_RUNTIME_ENTRY(NonBoolTypeError, 1) {
  const intptr_t location = GetCallerLocation();
  const Instance& src_instance = Instance::CheckedHandle(arguments.ArgAt(0));
  ASSERT(src_instance.IsNull() || !src_instance.IsBool());
  const Type& bool_interface = Type::Handle(Type::BoolType());
  const AbstractType& src_type = AbstractType::Handle(src_instance.GetType());
  const String& src_type_name = String::Handle(src_type.UserVisibleName());
  const String& bool_type_name =
      String::Handle(bool_interface.UserVisibleName());
  const String& no_bound_error = String::Handle();
  Exceptions::CreateAndThrowTypeError(location, src_type_name, bool_type_name,
                                      Symbols::BooleanExpression(),
                                      no_bound_error);
  UNREACHABLE();
}


// Report that the type of the type check is malformed or malbounded.
// Arg0: src value.
// Arg1: name of destination being assigned to.
// Arg2: type of destination being assigned to.
// Return value: none, throws an exception.
DEFINE_RUNTIME_ENTRY(BadTypeError, 3) {
  const intptr_t location = GetCallerLocation();
  const Instance& src_value = Instance::CheckedHandle(arguments.ArgAt(0));
  const String& dst_name = String::CheckedHandle(arguments.ArgAt(1));
  const AbstractType& dst_type =
      AbstractType::CheckedHandle(arguments.ArgAt(2));
  const AbstractType& src_type = AbstractType::Handle(src_value.GetType());
  const String& src_type_name = String::Handle(src_type.UserVisibleName());

  String& dst_type_name = String::Handle();
  LanguageError& error = LanguageError::Handle(dst_type.error());
  ASSERT(!error.IsNull());
  if (error.kind() == LanguageError::kMalformedType) {
    dst_type_name = Symbols::Malformed().raw();
  } else {
    ASSERT(error.kind() == LanguageError::kMalboundedType);
    dst_type_name = Symbols::Malbounded().raw();
  }
  const String& error_message = String::ZoneHandle(
      Symbols::New(error.ToErrorCString()));
  Exceptions::CreateAndThrowTypeError(
      location, src_type_name, dst_type_name, dst_name, error_message);
  UNREACHABLE();
}


DEFINE_RUNTIME_ENTRY(Throw, 1) {
  const Instance& exception = Instance::CheckedHandle(arguments.ArgAt(0));
  Exceptions::Throw(exception);
}


DEFINE_RUNTIME_ENTRY(ReThrow, 2) {
  const Instance& exception = Instance::CheckedHandle(arguments.ArgAt(0));
  const Instance& stacktrace = Instance::CheckedHandle(arguments.ArgAt(1));
  Exceptions::ReThrow(exception, stacktrace);
}


// Patches static call in optimized code with the target's entry point.
// Compiles target if necessary.
DEFINE_RUNTIME_ENTRY(PatchStaticCall, 0) {
  DartFrameIterator iterator;
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);
  const Code& caller_code = Code::Handle(caller_frame->LookupDartCode());
  ASSERT(!caller_code.IsNull());
  ASSERT(caller_code.is_optimized());
  const Function& target_function = Function::Handle(
      caller_code.GetStaticCallTargetFunctionAt(caller_frame->pc()));
  if (!target_function.HasCode()) {
    const Error& error =
        Error::Handle(Compiler::CompileFunction(target_function));
    if (!error.IsNull()) {
      Exceptions::PropagateError(error);
    }
  }
  const Code& target_code = Code::Handle(target_function.CurrentCode());
  // Before patching verify that we are not repeatedly patching to the same
  // target.
  ASSERT(target_code.EntryPoint() !=
         CodePatcher::GetStaticCallTargetAt(caller_frame->pc(), caller_code));
  CodePatcher::PatchStaticCallAt(caller_frame->pc(), caller_code,
                                 target_code.EntryPoint());
  caller_code.SetStaticCallTargetCodeAt(caller_frame->pc(), target_code);
  if (FLAG_trace_patching) {
    OS::PrintErr("PatchStaticCall: patching from %#" Px " to '%s' %#" Px "\n",
        caller_frame->pc(),
        target_function.ToFullyQualifiedCString(),
        target_code.EntryPoint());
  }
  arguments.SetReturn(target_code);
}


// Resolves and compiles the target function of an instance call, updates
// function cache of the receiver's class and returns the compiled code or null.
// Only the number of named arguments is checked, but not the actual names.
RawCode* ResolveCompileInstanceCallTarget(const Instance& receiver,
                                          const ICData& ic_data) {
  ArgumentsDescriptor
      arguments_descriptor(Array::Handle(ic_data.arguments_descriptor()));
  String& function_name = String::Handle(ic_data.target_name());
  ASSERT(function_name.IsSymbol());

  Function& function = Function::Handle();
  function = Resolver::ResolveDynamic(receiver,
                                      function_name,
                                      arguments_descriptor);
  if (function.IsNull()) {
    return Code::null();
  } else {
    if (!function.HasCode()) {
      const Error& error = Error::Handle(Compiler::CompileFunction(function));
      if (!error.IsNull()) {
        Exceptions::PropagateError(error);
      }
    }
    return function.CurrentCode();
  }
}


// Result of an invoke may be an unhandled exception, in which case we
// rethrow it.
static void CheckResultError(const Object& result) {
  if (result.IsError()) {
    Exceptions::PropagateError(Error::Cast(result));
  }
}


// Gets called from debug stub when code reaches a breakpoint
// set on a runtime stub call.
DEFINE_RUNTIME_ENTRY(BreakpointRuntimeHandler, 0) {
  ASSERT(isolate->debugger() != NULL);
  DartFrameIterator iterator;
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);
  uword orig_stub =
      isolate->debugger()->GetPatchedStubAddress(caller_frame->pc());
  isolate->debugger()->SignalBpReached();
  ASSERT((orig_stub & kSmiTagMask) == kSmiTag);
  arguments.SetReturn(Smi::Handle(reinterpret_cast<RawSmi*>(orig_stub)));
}


// Gets called from debug stub when code reaches a breakpoint.
DEFINE_RUNTIME_ENTRY(BreakpointStaticHandler, 0) {
  ASSERT(isolate->debugger() != NULL);
  isolate->debugger()->SignalBpReached();
  // Make sure the static function that is about to be called is
  // compiled. The stub will jump to the entry point without any
  // further tests.
  DartFrameIterator iterator;
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);
  const Code& code = Code::Handle(caller_frame->LookupDartCode());
  ASSERT(!code.is_optimized());
  const Function& function =
      Function::Handle(CodePatcher::GetUnoptimizedStaticCallAt(
          caller_frame->pc(), code, NULL));

  if (!function.HasCode()) {
    const Error& error = Error::Handle(Compiler::CompileFunction(function));
    if (!error.IsNull()) {
      Exceptions::PropagateError(error);
    }
  }
  arguments.SetReturn(Code::ZoneHandle(function.CurrentCode()));
}


// Gets called from debug stub when code reaches a breakpoint at a return
// in Dart code.
DEFINE_RUNTIME_ENTRY(BreakpointReturnHandler, 0) {
  ASSERT(isolate->debugger() != NULL);
  isolate->debugger()->SignalBpReached();
}


// Gets called from debug stub when code reaches a breakpoint.
DEFINE_RUNTIME_ENTRY(BreakpointDynamicHandler, 0) {
  ASSERT(isolate->debugger() != NULL);
  isolate->debugger()->SignalBpReached();
}


DEFINE_RUNTIME_ENTRY(SingleStepHandler, 0) {
  ASSERT(isolate->debugger() != NULL);
  isolate->debugger()->SingleStepCallback();
}


static RawFunction* InlineCacheMissHandler(
    const GrowableArray<const Instance*>& args,
    const ICData& ic_data) {
  const Instance& receiver = *args[0];
  const Code& target_code =
      Code::Handle(ResolveCompileInstanceCallTarget(receiver, ic_data));
  if (target_code.IsNull()) {
    // Let the megamorphic stub handle special cases: NoSuchMethod,
    // closure calls.
    if (FLAG_trace_ic) {
      OS::PrintErr("InlineCacheMissHandler NULL code for %s receiver: %s\n",
                   String::Handle(ic_data.target_name()).ToCString(),
                   receiver.ToCString());
    }
    return Function::null();
  }
  const Function& target_function =
      Function::Handle(target_code.function());
  ASSERT(!target_function.IsNull());
  if (args.length() == 1) {
    ic_data.AddReceiverCheck(args[0]->GetClassId(), target_function);
  } else {
    GrowableArray<intptr_t> class_ids(args.length());
    ASSERT(ic_data.num_args_tested() == args.length());
    for (intptr_t i = 0; i < args.length(); i++) {
      class_ids.Add(args[i]->GetClassId());
    }
    ic_data.AddCheck(class_ids, target_function);
  }
  if (FLAG_trace_ic_miss_in_optimized || FLAG_trace_ic) {
    DartFrameIterator iterator;
    StackFrame* caller_frame = iterator.NextFrame();
    ASSERT(caller_frame != NULL);
    if (FLAG_trace_ic_miss_in_optimized) {
      const Code& caller = Code::Handle(Code::LookupCode(caller_frame->pc()));
      if (caller.is_optimized()) {
        OS::PrintErr("IC miss in optimized code; call %s -> %s\n",
            Function::Handle(caller.function()).ToCString(),
            target_function.ToCString());
      }
    }
    if (FLAG_trace_ic) {
      OS::PrintErr("InlineCacheMissHandler %" Pd " call at %#" Px "' "
                   "adding <%s> id:%" Pd " -> <%s>\n",
          args.length(),
          caller_frame->pc(),
          Class::Handle(receiver.clazz()).ToCString(),
          receiver.GetClassId(),
          target_function.ToCString());
    }
  }
  return target_function.raw();
}


// Handles inline cache misses by updating the IC data array of the call
// site.
//   Arg0: Receiver object.
//   Arg1: IC data object.
//   Returns: target function with compiled code or null.
// Modifies the instance call to hold the updated IC data array.
DEFINE_RUNTIME_ENTRY(InlineCacheMissHandlerOneArg, 2) {
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(1));
  GrowableArray<const Instance*> args(1);
  args.Add(&receiver);
  const Function& result =
      Function::Handle(InlineCacheMissHandler(args, ic_data));
  arguments.SetReturn(result);
}


// Handles inline cache misses by updating the IC data array of the call
// site.
//   Arg0: Receiver object.
//   Arg1: Argument after receiver.
//   Arg2: IC data object.
//   Returns: target function with compiled code or null.
// Modifies the instance call to hold the updated IC data array.
DEFINE_RUNTIME_ENTRY(InlineCacheMissHandlerTwoArgs, 3) {
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const Instance& other = Instance::CheckedHandle(arguments.ArgAt(1));
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(2));
  GrowableArray<const Instance*> args(2);
  args.Add(&receiver);
  args.Add(&other);
  const Function& result =
      Function::Handle(InlineCacheMissHandler(args, ic_data));
  arguments.SetReturn(result);
}


// Handles inline cache misses by updating the IC data array of the call
// site.
//   Arg0: Receiver object.
//   Arg1: Argument after receiver.
//   Arg2: Second argument after receiver.
//   Arg3: IC data object.
//   Returns: target function with compiled code or null.
// Modifies the instance call to hold the updated IC data array.
DEFINE_RUNTIME_ENTRY(InlineCacheMissHandlerThreeArgs, 4) {
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const Instance& arg1 = Instance::CheckedHandle(arguments.ArgAt(1));
  const Instance& arg2 = Instance::CheckedHandle(arguments.ArgAt(2));
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(3));
  GrowableArray<const Instance*> args(3);
  args.Add(&receiver);
  args.Add(&arg1);
  args.Add(&arg2);
  const Function& result =
      Function::Handle(InlineCacheMissHandler(args, ic_data));
  arguments.SetReturn(result);
}


// Handles a static call in unoptimized code that has two argument types not
// seen before. Compile the target if necessary and update the ICData.
// Arg0: argument 0.
// Arg1: argument 1.
// Arg2: IC data object.
DEFINE_RUNTIME_ENTRY(StaticCallMissHandlerTwoArgs, 3) {
  const Instance& arg0 = Instance::CheckedHandle(arguments.ArgAt(0));
  const Instance& arg1 = Instance::CheckedHandle(arguments.ArgAt(1));
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(2));
  // IC data for static call is prepopulated with the statically known target.
  ASSERT(ic_data.NumberOfChecks() > 0);
  const Function& target = Function::Handle(ic_data.GetTargetAt(0));
  if (!target.HasCode()) {
    const Error& error = Error::Handle(Compiler::CompileFunction(target));
    if (!error.IsNull()) {
      Exceptions::PropagateError(error);
    }
  }
  ASSERT(!target.IsNull() && target.HasCode());
  GrowableArray<intptr_t> cids(2);
  cids.Add(arg0.GetClassId());
  cids.Add(arg1.GetClassId());
  ic_data.AddCheck(cids, target);
  if (FLAG_trace_ic) {
    DartFrameIterator iterator;
    StackFrame* caller_frame = iterator.NextFrame();
    ASSERT(caller_frame != NULL);
    OS::PrintErr("StaticCallMissHandler at %#" Px
                 " target %s (%" Pd ", %" Pd ")\n",
                 caller_frame->pc(), target.ToCString(), cids[0], cids[1]);
  }
  arguments.SetReturn(target);
}


// Handle a miss of a megamorphic cache.
//   Arg0: Receiver.
//   Arg1: ICData object.
//   Arg2: Arguments descriptor array.

//   Returns: target instructions to call or null if the
// InstanceFunctionLookup stub should be used (e.g., to invoke no such
// method and implicit closures)..
DEFINE_RUNTIME_ENTRY(MegamorphicCacheMissHandler, 3) {
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(1));
  const Array& descriptor = Array::CheckedHandle(arguments.ArgAt(2));
  const String& name = String::Handle(ic_data.target_name());
  const MegamorphicCache& cache = MegamorphicCache::Handle(
      isolate->megamorphic_cache_table()->Lookup(name, descriptor));
  Class& cls = Class::Handle(receiver.clazz());
  ASSERT(!cls.IsNull());
  if (FLAG_trace_ic || FLAG_trace_ic_miss_in_optimized) {
    OS::PrintErr("Megamorphic IC miss, class=%s, function=%s\n",
                 cls.ToCString(), name.ToCString());
  }

  ArgumentsDescriptor args_desc(descriptor);
  const Function& target = Function::Handle(
      Resolver::ResolveDynamicForReceiverClass(cls,
                                               name,
                                               args_desc));

  Instructions& instructions = Instructions::Handle();
  if (!target.IsNull()) {
    if (!target.HasCode()) {
      const Error& error = Error::Handle(Compiler::CompileFunction(target));
      if (!error.IsNull()) {
        Exceptions::PropagateError(error);
      }
    }
    ASSERT(target.HasCode());
    instructions = Code::Handle(target.CurrentCode()).instructions();
  }
  arguments.SetReturn(instructions);
  if (instructions.IsNull()) return;

  cache.EnsureCapacity();
  const Smi& class_id = Smi::Handle(Smi::New(cls.id()));
  cache.Insert(class_id, target);
  return;
}


// Updates IC data for two arguments. Used by the equality operation when
// the control flow bypasses regular inline cache (null arguments).
//   Arg0: Receiver object.
//   Arg1: Argument after receiver.
//   Arg2: Target's name.
//   Arg3: ICData.
DEFINE_RUNTIME_ENTRY(UpdateICDataTwoArgs, 4) {
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const Instance& arg1 = Instance::CheckedHandle(arguments.ArgAt(1));
  const String& target_name = String::CheckedHandle(arguments.ArgAt(2));
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(3));
  GrowableArray<const Instance*> args(2);
  args.Add(&receiver);
  args.Add(&arg1);
  const intptr_t kNumArguments = 2;
    ArgumentsDescriptor args_desc(
        Array::Handle(ArgumentsDescriptor::New(kNumArguments)));
  const Function& target_function = Function::Handle(
      Resolver::ResolveDynamic(receiver,
                               target_name,
                               args_desc));
  ASSERT(!target_function.IsNull());
  GrowableArray<intptr_t> class_ids(kNumArguments);
  ASSERT(ic_data.num_args_tested() == kNumArguments);
  class_ids.Add(receiver.GetClassId());
  class_ids.Add(arg1.GetClassId());
  ic_data.AddCheck(class_ids, target_function);
}


// Invoke appropriate noSuchMethod function.
// Arg0: receiver.
// Arg1: ic-data.
// Arg2: arguments descriptor array.
// Arg3: arguments array.
DEFINE_RUNTIME_ENTRY(InvokeNoSuchMethodFunction, 4) {
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(1));
  const Array& orig_arguments_desc = Array::CheckedHandle(arguments.ArgAt(2));
  const Array& orig_arguments = Array::CheckedHandle(arguments.ArgAt(3));

  String& original_function_name = String::Handle(ic_data.target_name());
  if (receiver.IsClosure()) {
    // For closure the function name is always 'call'. Replace it with the
    // name of the closurized function so that exception contains more
    // relevant information.
    const Function& function = Function::Handle(Closure::function(receiver));
    original_function_name = function.QualifiedUserVisibleName();
  }
  const Object& result = Object::Handle(
      DartEntry::InvokeNoSuchMethod(receiver,
                                    original_function_name,
                                    orig_arguments,
                                    orig_arguments_desc));
  CheckResultError(result);
  arguments.SetReturn(result);
}


// A non-closure object was invoked as a closure, so call the "call" method
// on it.
// Arg0: arguments descriptor.
// Arg1: arguments array, including non-closure object.
DEFINE_RUNTIME_ENTRY(InvokeNonClosure, 2) {
  const Array& args_descriptor = Array::CheckedHandle(arguments.ArgAt(0));
  const Array& function_args = Array::CheckedHandle(arguments.ArgAt(1));

  const Object& result = Object::Handle(
      DartEntry::InvokeClosure(function_args, args_descriptor));
  CheckResultError(result);
  arguments.SetReturn(result);
}


// An instance call of the form o.f(...) could not be resolved.  Check if
// there is a getter with the same name.  If so, invoke it.  If the value is
// a closure, invoke it with the given arguments.  If the value is a
// non-closure, attempt to invoke "call" on it.
static bool ResolveCallThroughGetter(const Instance& receiver,
                                     const Class& receiver_class,
                                     const String& target_name,
                                     const Array& arguments_descriptor,
                                     const Array& arguments,
                                     const ICData& ic_data,
                                     Object* result) {
  // 1. Check if there is a getter with the same name.
  const String& getter_name = String::Handle(Field::GetterName(target_name));
  const int kNumArguments = 1;
  ArgumentsDescriptor args_desc(
      Array::Handle(ArgumentsDescriptor::New(kNumArguments)));
  const Function& getter = Function::Handle(
      Resolver::ResolveDynamicForReceiverClass(receiver_class,
                                               getter_name,
                                               args_desc));
  if (getter.IsNull() || getter.IsMethodExtractor()) {
    return false;
  }

  const Function& target_function =
      Function::Handle(receiver_class.GetInvocationDispatcher(
          target_name,
          arguments_descriptor,
          RawFunction::kInvokeFieldDispatcher));
  // Update IC data.
  ASSERT(!target_function.IsNull());
  ic_data.AddReceiverCheck(receiver.GetClassId(), target_function);
  if (FLAG_trace_ic) {
    OS::PrintErr("InvokeField IC miss: adding <%s> id:%" Pd " -> <%s>\n",
        Class::Handle(receiver.clazz()).ToCString(),
        receiver.GetClassId(),
        target_function.ToCString());
  }
  *result = DartEntry::InvokeFunction(target_function,
                                      arguments,
                                      arguments_descriptor);
  CheckResultError(*result);
  return true;
}


// The IC miss handler has failed to find a (cacheable) instance function to
// invoke.  Handle three possibilities:
//
// 1. If the call was a getter o.f, there may be an instance function with
//    the same name.  If so, create an implicit closure and return it.
//
// 2. If the call was an instance call o.f(...), there may be a getter with
//    the same name.  If so, invoke it.  If the value is a closure, invoke
//    it with the given arguments.  If the value is a non-closure, attempt
//    to invoke "call" on it.
//
// 3. There is no such method.
DEFINE_RUNTIME_ENTRY(InstanceFunctionLookup, 4) {
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(1));
  const Array& args_descriptor = Array::CheckedHandle(arguments.ArgAt(2));
  const Array& args = Array::CheckedHandle(arguments.ArgAt(3));

  const Class& receiver_class = Class::Handle(receiver.clazz());
  const String& target_name = String::Handle(ic_data.target_name());

  Object& result = Object::Handle();
  if (!ResolveCallThroughGetter(receiver,
                                receiver_class,
                                target_name,
                                args_descriptor,
                                args,
                                ic_data,
                                &result)) {
    ArgumentsDescriptor desc(args_descriptor);
    const Function& target_function =
        Function::Handle(receiver_class.GetInvocationDispatcher(
            target_name,
            args_descriptor,
            RawFunction::kNoSuchMethodDispatcher));
    // Update IC data.
    ASSERT(!target_function.IsNull());
    intptr_t receiver_cid = receiver.GetClassId();
    if (ic_data.num_args_tested() == 1) {
      // In optimized code we may enter into here via the
      // MegamorphicCacheMissHandler since noSuchMethod dispatchers are not
      // inserted into the megamorphic cache. Therefore, we need to guard
      // against entering the same check twice into the ICData.
      // Note that num_args_tested == 1 in optimized code.
      // TODO(fschneider): Handle extraordinary cases like noSuchMethod and
      // implicit closure invocation properly in the megamorphic cache.
      const Function& target =
          Function::Handle(ic_data.GetTargetForReceiverClassId(receiver_cid));
      if (target.IsNull()) {
        ic_data.AddReceiverCheck(receiver_cid, target_function);
      }
    } else {
      // Operators calls have two or three arguments tested ([], []=, etc.)
      ASSERT(ic_data.num_args_tested() > 1);
      GrowableArray<intptr_t> class_ids(ic_data.num_args_tested());
      class_ids.Add(receiver_cid);
      for (intptr_t i = 1; i < ic_data.num_args_tested(); ++i) {
        class_ids.Add(Object::Handle(args.At(i)).GetClassId());
      }
      ic_data.AddCheck(class_ids, target_function);
    }
    if (FLAG_trace_ic) {
      OS::PrintErr("NoSuchMethod IC miss: adding <%s> id:%" Pd " -> <%s>\n",
          Class::Handle(receiver.clazz()).ToCString(),
          receiver_cid,
          target_function.ToCString());
    }
    result = DartEntry::InvokeFunction(target_function, args, args_descriptor);
  }
  CheckResultError(result);
  arguments.SetReturn(result);
}


static bool CanOptimizeFunction(const Function& function, Isolate* isolate) {
  const intptr_t kLowInvocationCount = -100000000;
  if (isolate->debugger()->IsStepping() ||
      isolate->debugger()->HasBreakpoint(function)) {
    // We cannot set breakpoints and single step in optimized code,
    // so do not optimize the function.
    function.set_usage_counter(0);
    return false;
  }
  if (function.deoptimization_counter() >=
      FLAG_deoptimization_counter_threshold) {
    if (FLAG_trace_failed_optimization_attempts ||
        FLAG_stop_on_excessive_deoptimization) {
      OS::PrintErr("Too Many Deoptimizations: %s\n",
          function.ToFullyQualifiedCString());
      if (FLAG_stop_on_excessive_deoptimization) {
        FATAL("Stop on excessive deoptimization");
      }
    }
    // TODO(srdjan): Investigate excessive deoptimization.
    function.set_usage_counter(kLowInvocationCount);
    return false;
  }
  if (FLAG_optimization_filter != NULL) {
    // FLAG_optimization_filter is a comma-separated list of strings that are
    // matched against the fully-qualified function name.
    char* save_ptr;  // Needed for strtok_r.
    const char* function_name = function.ToFullyQualifiedCString();
    intptr_t len = strlen(FLAG_optimization_filter) + 1;  // Length with \0.
    char* filter = new char[len];
    strncpy(filter, FLAG_optimization_filter, len);  // strtok modifies arg 1.
    char* token = strtok_r(filter, ",", &save_ptr);
    bool found = false;
    while (token != NULL) {
      if (strstr(function_name, token) != NULL) {
        found = true;
        break;
      }
      token = strtok_r(NULL, ",", &save_ptr);
    }
    delete[] filter;
    if (!found) {
      function.set_usage_counter(kLowInvocationCount);
      return false;
    }
  }
  if (!function.is_optimizable()) {
    if (FLAG_trace_failed_optimization_attempts) {
      OS::PrintErr("Not Optimizable: %s\n", function.ToFullyQualifiedCString());
    }
    // TODO(5442338): Abort as this should not happen.
    function.set_usage_counter(kLowInvocationCount);
    return false;
  }
  return true;
}


DEFINE_RUNTIME_ENTRY(StackOverflow, 0) {
#if defined(USING_SIMULATOR)
  uword stack_pos = Simulator::Current()->get_register(SPREG);
#else
  uword stack_pos = reinterpret_cast<uword>(&arguments);
#endif

  // If an interrupt happens at the same time as a stack overflow, we
  // process the stack overflow first.
  if (stack_pos < isolate->saved_stack_limit()) {
    // Use the preallocated stack overflow exception to avoid calling
    // into dart code.
    const Instance& exception =
        Instance::Handle(isolate->object_store()->stack_overflow());
    Exceptions::Throw(exception);
    UNREACHABLE();
  }

  uword interrupt_bits = isolate->GetAndClearInterrupts();
  if (interrupt_bits & Isolate::kStoreBufferInterrupt) {
    if (FLAG_verbose_gc) {
      OS::PrintErr("Scavenge scheduled by store buffer overflow.\n");
    }
    isolate->heap()->CollectGarbage(Heap::kNew);
  }
  if (interrupt_bits & Isolate::kMessageInterrupt) {
    isolate->message_handler()->HandleOOBMessages();
  }
  if (interrupt_bits & Isolate::kApiInterrupt) {
    // Signal isolate interrupt  event.
    Debugger::SignalIsolateEvent(Debugger::kIsolateInterrupted);

    Dart_IsolateInterruptCallback callback = isolate->InterruptCallback();
    if (callback) {
      if ((*callback)()) {
        return;
      } else {
        // TODO(turnidge): Unwind the stack.
        UNIMPLEMENTED();
      }
    }
  }
  if (interrupt_bits & Isolate::kVmStatusInterrupt) {
    Dart_IsolateInterruptCallback callback = isolate->VmStatsCallback();
    if (callback) {
      (*callback)();
    }
  }

  if (FLAG_use_osr && (interrupt_bits == 0)) {
    DartFrameIterator iterator;
    StackFrame* frame = iterator.NextFrame();
    const Function& function = Function::Handle(frame->LookupDartFunction());
    ASSERT(!function.IsNull());
    if (!CanOptimizeFunction(function, isolate)) return;
    intptr_t osr_id =
        Code::Handle(function.unoptimized_code()).GetDeoptIdForOsr(frame->pc());
    if (FLAG_trace_osr) {
      OS::Print("Attempting OSR for %s at id=%" Pd ", count=%" Pd "\n",
                function.ToFullyQualifiedCString(),
                osr_id,
                function.usage_counter());
    }

    const Code& original_code = Code::Handle(function.CurrentCode());
    const Error& error =
        Error::Handle(Compiler::CompileOptimizedFunction(function, osr_id));
    if (!error.IsNull()) Exceptions::PropagateError(error);

    const Code& optimized_code = Code::Handle(function.CurrentCode());
    // The current code will not be changed in the case that the compiler
    // bailed out during OSR compilation.
    if (optimized_code.raw() != original_code.raw()) {
      // The OSR code does not work for calling the function, so restore the
      // unoptimized code.  Patch the stack frame to return into the OSR
      // code.
      uword optimized_entry =
          Instructions::Handle(optimized_code.instructions()).EntryPoint();
      function.SetCode(original_code);
      frame->set_pc(optimized_entry);
    }
  }
}


DEFINE_RUNTIME_ENTRY(TraceICCall, 2) {
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(0));
  const Function& function = Function::CheckedHandle(arguments.ArgAt(1));
  DartFrameIterator iterator;
  StackFrame* frame = iterator.NextFrame();
  ASSERT(frame != NULL);
  OS::PrintErr("IC call @%#" Px ": ICData: %p cnt:%" Pd " nchecks: %" Pd
      " %s %s\n",
      frame->pc(),
      ic_data.raw(),
      function.usage_counter(),
      ic_data.NumberOfChecks(),
      ic_data.is_closure_call() ? "closure" : "",
      function.ToFullyQualifiedCString());
}


// This is called from function that needs to be optimized.
// The requesting function can be already optimized (reoptimization).
// Returns the Code object where to continue execution.
DEFINE_RUNTIME_ENTRY(OptimizeInvokedFunction, 1) {
  const Function& function = Function::CheckedHandle(arguments.ArgAt(0));
  ASSERT(!function.IsNull());
  ASSERT(function.HasCode());

  if (CanOptimizeFunction(function, isolate)) {
    const Error& error =
        Error::Handle(Compiler::CompileOptimizedFunction(function));
    if (!error.IsNull()) {
      Exceptions::PropagateError(error);
    }
    const Code& optimized_code = Code::Handle(function.CurrentCode());
    ASSERT(!optimized_code.IsNull());
    // Reset usage counter for reoptimization.
    function.set_usage_counter(0);
  }
  arguments.SetReturn(Code::Handle(function.CurrentCode()));
}


// The caller must be a static call in a Dart frame, or an entry frame.
// Patch static call to point to valid code's entry point.
DEFINE_RUNTIME_ENTRY(FixCallersTarget, 0) {
  StackFrameIterator iterator(StackFrameIterator::kDontValidateFrames);
  StackFrame* frame = iterator.NextFrame();
  while (frame != NULL && (frame->IsStubFrame() || frame->IsExitFrame())) {
    frame = iterator.NextFrame();
  }
  ASSERT(frame != NULL);
  if (frame->IsEntryFrame()) {
    // Since function's current code is always unpatched, the entry frame always
    // calls to unpatched code.
    UNREACHABLE();
  }
  ASSERT(frame->IsDartFrame());
  const Code& caller_code = Code::Handle(frame->LookupDartCode());
  ASSERT(caller_code.is_optimized());
  const Function& target_function = Function::Handle(
      caller_code.GetStaticCallTargetFunctionAt(frame->pc()));

  // Check whether the code object has been detached from the target function.
  // If it has been detached, reattach it.
  Code& target_code = Code::Handle();
  if (target_function.HasCode()) {
    target_code ^= target_function.CurrentCode();
    CodePatcher::PatchStaticCallAt(frame->pc(), caller_code,
                                   target_code.EntryPoint());
    caller_code.SetStaticCallTargetCodeAt(frame->pc(), target_code);
  } else {
    ASSERT(target_function.unoptimized_code() == Code::null());
    target_code ^= caller_code.GetStaticCallTargetCodeAt(frame->pc());
    ASSERT(!target_code.IsNull());
    ASSERT(!target_code.is_optimized());
    target_function.ReattachCode(target_code);
  }
  if (FLAG_trace_patching) {
    OS::PrintErr("FixCallersTarget: patching from %#" Px " to '%s' %#" Px "\n",
        frame->pc(),
        Function::Handle(target_code.function()).ToFullyQualifiedCString(),
        target_code.EntryPoint());
  }
  arguments.SetReturn(target_code);
  ASSERT(target_function.HasCode());
}


const char* DeoptReasonToText(intptr_t deopt_id) {
  switch (deopt_id) {
#define DEOPT_REASON_ID_TO_TEXT(name) case kDeopt##name: return #name;
DEOPT_REASONS(DEOPT_REASON_ID_TO_TEXT)
#undef DEOPT_REASON_ID_TO_TEXT
    default:
      UNREACHABLE();
      return "";
  }
}


void DeoptimizeAt(const Code& optimized_code, uword pc) {
  ASSERT(optimized_code.is_optimized());
  intptr_t deopt_reason = kDeoptUnknown;
  const DeoptInfo& deopt_info =
      DeoptInfo::Handle(optimized_code.GetDeoptInfoAtPc(pc, &deopt_reason));
  ASSERT(!deopt_info.IsNull());
  const Function& function = Function::Handle(optimized_code.function());
  const Code& unoptimized_code = Code::Handle(function.unoptimized_code());
  ASSERT(!unoptimized_code.IsNull());
  // The switch to unoptimized code may have already occured.
  if (function.HasOptimizedCode()) {
    function.SwitchToUnoptimizedCode();
  }
  // Patch call site (lazy deoptimization is quite rare, patching it twice
  // is not a performance issue).
  uword lazy_deopt_jump = optimized_code.GetLazyDeoptPc();
  ASSERT(lazy_deopt_jump != 0);
  CodePatcher::InsertCallAt(pc, lazy_deopt_jump);
  // Mark code as dead (do not GC its embedded objects).
  optimized_code.set_is_alive(false);
}


// Currently checks only that all optimized frames have kDeoptIndex
// and unoptimized code has the kDeoptAfter.
void DeoptimizeAll() {
  DartFrameIterator iterator;
  StackFrame* frame = iterator.NextFrame();
  Code& optimized_code = Code::Handle();
  while (frame != NULL) {
    optimized_code = frame->LookupDartCode();
    if (optimized_code.is_optimized()) {
      DeoptimizeAt(optimized_code, frame->pc());
    }
    frame = iterator.NextFrame();
  }
}


// Returns true if the given array of cids contains the given cid.
static bool ContainsCid(const GrowableArray<intptr_t>& cids, intptr_t cid) {
  for (intptr_t i = 0; i < cids.length(); i++) {
    if (cids[i] == cid) {
      return true;
    }
  }
  return false;
}


// Deoptimize optimized code on stack if its class is in the 'classes' array.
void DeoptimizeIfOwner(const GrowableArray<intptr_t>& classes) {
  DartFrameIterator iterator;
  StackFrame* frame = iterator.NextFrame();
  Code& optimized_code = Code::Handle();
  while (frame != NULL) {
    optimized_code = frame->LookupDartCode();
    if (optimized_code.is_optimized()) {
      const intptr_t owner_cid = Class::Handle(Function::Handle(
          optimized_code.function()).Owner()).id();
      if (ContainsCid(classes, owner_cid)) {
        DeoptimizeAt(optimized_code, frame->pc());
      }
    }
  }
}


static void CopySavedRegisters(uword saved_registers_address,
                               fpu_register_t** fpu_registers,
                               intptr_t** cpu_registers) {
  ASSERT(sizeof(fpu_register_t) == kFpuRegisterSize);
  fpu_register_t* fpu_registers_copy =
      new fpu_register_t[kNumberOfFpuRegisters];
  ASSERT(fpu_registers_copy != NULL);
  for (intptr_t i = 0; i < kNumberOfFpuRegisters; i++) {
    fpu_registers_copy[i] =
        *reinterpret_cast<fpu_register_t*>(saved_registers_address);
    saved_registers_address += kFpuRegisterSize;
  }
  *fpu_registers = fpu_registers_copy;

  ASSERT(sizeof(intptr_t) == kWordSize);
  intptr_t* cpu_registers_copy = new intptr_t[kNumberOfCpuRegisters];
  ASSERT(cpu_registers_copy != NULL);
  for (intptr_t i = 0; i < kNumberOfCpuRegisters; i++) {
    cpu_registers_copy[i] =
        *reinterpret_cast<intptr_t*>(saved_registers_address);
    saved_registers_address += kWordSize;
  }
  *cpu_registers = cpu_registers_copy;
}


// Copies saved registers and caller's frame into temporary buffers.
// Returns the stack size of unoptimized frame.
DEFINE_LEAF_RUNTIME_ENTRY(intptr_t, DeoptimizeCopyFrame,
                          1, uword saved_registers_address) {
  Isolate* isolate = Isolate::Current();
  StackZone zone(isolate);
  HANDLESCOPE(isolate);

  // All registers have been saved below last-fp as if they were locals.
  const uword last_fp = saved_registers_address
                        + (kNumberOfCpuRegisters * kWordSize)
                        + (kNumberOfFpuRegisters * kFpuRegisterSize)
                        - ((kFirstLocalSlotFromFp + 1) * kWordSize);

  // Get optimized code and frame that need to be deoptimized.
  DartFrameIterator iterator(last_fp);
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);
  const Code& optimized_code = Code::Handle(caller_frame->LookupDartCode());
  ASSERT(optimized_code.is_optimized());

  // Copy the saved registers from the stack.
  fpu_register_t* fpu_registers;
  intptr_t* cpu_registers;
  CopySavedRegisters(saved_registers_address, &fpu_registers, &cpu_registers);

  // Create the DeoptContext.
  DeoptContext* deopt_context =
      new DeoptContext(caller_frame, optimized_code,
                       DeoptContext::kDestIsOriginalFrame,
                       fpu_registers, cpu_registers);
  isolate->set_deopt_context(deopt_context);

  // Stack size (FP - SP) in bytes.
  return deopt_context->DestStackAdjustment() * kWordSize;
}
END_LEAF_RUNTIME_ENTRY


// The stack has been adjusted to fit all values for unoptimized frame.
// Fill the unoptimized frame.
DEFINE_LEAF_RUNTIME_ENTRY(void, DeoptimizeFillFrame, 1, uword last_fp) {
  Isolate* isolate = Isolate::Current();
  StackZone zone(isolate);
  HANDLESCOPE(isolate);

  DeoptContext* deopt_context = isolate->deopt_context();
  DartFrameIterator iterator(last_fp);
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);

#if defined(DEBUG)
  {
    // The code from the deopt_context.
    const Code& code = Code::Handle(deopt_context->code());

    // The code from our frame.
    const Code& optimized_code = Code::Handle(caller_frame->LookupDartCode());
    const Function& function = Function::Handle(optimized_code.function());
    ASSERT(!function.IsNull());

    // The code will be the same as before.
    ASSERT(code.raw() == optimized_code.raw());

    // Some sanity checking of the optimized/unoptimized code.
    const Code& unoptimized_code = Code::Handle(function.unoptimized_code());
    ASSERT(!optimized_code.IsNull() && optimized_code.is_optimized());
    ASSERT(!unoptimized_code.IsNull() && !unoptimized_code.is_optimized());
  }
#endif

  // TODO(turnidge): Compute the start of the dest frame in the
  // DeoptContext instead of passing it in here.
  intptr_t* start = reinterpret_cast<intptr_t*>(
      caller_frame->sp() - (kDartFrameFixedSize * kWordSize));
  deopt_context->set_dest_frame(start);
  deopt_context->FillDestFrame();
}
END_LEAF_RUNTIME_ENTRY


// This is the last step in the deoptimization, GC can occur.
// Returns number of bytes to remove from the expression stack of the
// bottom-most deoptimized frame. Those arguments were artificially injected
// under return address to keep them discoverable by GC that can occur during
// materialization phase.
DEFINE_RUNTIME_ENTRY(DeoptimizeMaterialize, 0) {
  DeoptContext* deopt_context = isolate->deopt_context();
  intptr_t deopt_arg_count = deopt_context->MaterializeDeferredObjects();
  isolate->set_deopt_context(NULL);
  delete deopt_context;

  // Return value tells deoptimization stub to remove the given number of bytes
  // from the stack.
  arguments.SetReturn(Smi::Handle(Smi::New(deopt_arg_count * kWordSize)));
}


DEFINE_LEAF_RUNTIME_ENTRY(intptr_t,
                          BigintCompare,
                          2,
                          RawBigint* left,
                          RawBigint* right) {
  Isolate* isolate = Isolate::Current();
  StackZone zone(isolate);
  HANDLESCOPE(isolate);
  const Bigint& big_left = Bigint::Handle(left);
  const Bigint& big_right = Bigint::Handle(right);
  return BigintOperations::Compare(big_left, big_right);
}
END_LEAF_RUNTIME_ENTRY


double DartModulo(double left, double right) {
  double remainder = fmod_ieee(left, right);
  if (remainder == 0.0) {
    // We explicitely switch to the positive 0.0 (just in case it was negative).
    remainder = +0.0;
  } else if (remainder < 0.0) {
    if (right < 0) {
      remainder -= right;
    } else {
      remainder += right;
    }
  }
  return remainder;
}


// Update global type feedback recorded for a field recording the assignment
// of the given value.
//   Arg0: Field object;
//   Arg1: Value that is being stored.
DEFINE_RUNTIME_ENTRY(UpdateFieldCid, 2) {
  const Field& field = Field::CheckedHandle(arguments.ArgAt(0));
  const Object& value = Object::Handle(arguments.ArgAt(1));
  field.UpdateGuardedCidAndLength(value);
}

}  // namespace dart
