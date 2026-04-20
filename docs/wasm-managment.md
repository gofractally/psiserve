We need to support the following features with WASM:

 mod.pzam = pzamCompile( mod.wasm, x86 | ARM | ... )  and is a fracpacked object 
       - it has slots for any number of architectures and compile flags
            (floating point, determinsitic, soft float, yield, gas, etc.. )
       - the .wasm exposes the .wit interface in both "binary" and schema form with comments
       - this step should be deterministic, and if LLVM is performed by dispatching the
            compile job to another wasm to sandbox and make determinsitic the compile;

 After compiling one .pzam, comes the linking step and there are three kinds of linking:
      * host - linking to host provided interfaces
      a. static - the modules share the same stack, heap, and linear memory
          - useful for trusted code bringing in trusted libraries with highest 
            performance and no copies. 
      b. isolated - the modules that have their own linear memory everything has to
            be copied from one module to the next
              
            - at link time the host verifies that the imports and exports have the same
              signature.
            - at run time, an effecient host lowers the args straight from one wasm's memory into
            another wasms memory
            - the signature cannot change or the link step will fail
      c. a module must somehow declare whether it allows shared or isolated linking and with whom
      d. the point of the "linking" is to pre-lookup the method indices so that dispatch can be fast
         at execution time.

Now an open question is whether two isolated modules can have independent host linking; and I think 
    the answer should be yes. In a smart contract they might have different Db permissions and all of
    that should be wired up once. 
            

    The host has different subsystems that have state, so these are linked by passing
    in a reference to that host state object, the type of the object determines the interface,
    and the impl/value is the instance that will be called on (the *this pointer) 


 Db hostDb;
 Fs hostFileSystem;

  i = new WasmInstance( mod.pzam ).linkHost( hostDb, hostFileSystem )
                                  .linkStatic( *.pzam )
                                  .linkIsolated( *.pzam );
  


 /// assuming the c++ code has header for ReflectedInterfaceType and 
 /// the mod.wasm actually exports that interface, .as<T>() returns a PSIO reflected proxy object
 /// that impliments a fast trampoline into the jited code and calls the guest method T::guestMethodName
 i->as<ReflectedInterfaceType>()->guestMethodName( params... );
      - assert if signature doesn't match when creating the proxy object returned by as<>

 i->init() - resets the linear memory of all linked modules to the initial state, all modules get the same host
             state.


    Now what happens when one Instance wants to call a method on another Instance! This isn't linking, each
    instance has its own permissions, and other host state and is fully isolated. Nevertheless, an instance
    does have an API in its exports... and a subset of thie API must be callable by other modules.

    The Host has a function that allows a guest to get a reference handle to the interface of another instance,
     and of course the *type* of this handle is declared in the interface. 

     aX = host->get( "accountX", "ExpectedInterface,eg ERC20" ) 
         returns a reference to an object impliments the ERC20 interface.. 
         host verifies that accountX exports the ERC20 interface and links up the methods...

     aX->transfer( ... ) 

    Now the problem is that linking an entire interface for a single method is likely overkill... so instead:

     func = host->resolve( "accountX", "transfer( from, to, ..)" );
     func->call( from, to, ... )

    and of course the one and done method:




 /// if the c++ code doesn't have the exact type of the method, then it must do
 /// a dynamic dispatch, but even this dynamic dispatch has a name and argument types sufficient
      to heavily optimize the dispatch if the transalation is captured in a lambda or the
      method is even jit itself... 

 result = genericInstance.call<R>( "guestMethodName", arg1, arg2, arg3 ) 
    - get the .pazam, look up the method name and signature
    - dynamically convert each arg to the type required by the signature
    - dispatch, and dynamically convert the result to R

 Now this is the issue, even this "so called" dynamic call is really static as
     all the expected types are known at compile type even if you don't say the
     interface name.

  
 Now the question remains what if a .wasm doesn't want to pre-link  

---

## Wire protocol: WIT is the default; fracpack-in-list<u8> is the evolution escape hatch

WIT + pre-JIT'd bridges is the single wire protocol for all cross-account
calls. Strict, sig-locked, memcpy-speed.

If a method wants message-passing / evolution semantics, it declares its
argument type as `list<u8>` and fracpacks inside. The bridge still marshals
strictly — but marshaling `list<u8>` is one `(ptr, len)` copy, which is the
fastest case anyway. The app does its own packing/unpacking at the ends.

Consequences:

- **One ABI, one dispatch path, one cache key shape.** No "dynamic call" or
  "generic call" primitive on the host. The host never needs to know about
  flexibility.
- **Proxies, routers, generic middleware** just export methods with
  `list<u8>` params. Expressible, typed at the method-name level, fast.
- **Mixed methods are fine**: `transfer(to: string, payload: list<u8>)`
  marshals `to` strictly and `payload` as an opaque blob. No special case.
- **Opting into flexibility costs nothing at the marshaling layer** — only
  the app's own fracpack work, which would have existed in message-passing
  models anyway.

The design decision: strict RPC is the only thing the host supports;
message passing is a pattern apps implement using strict RPC as the
carrier.

