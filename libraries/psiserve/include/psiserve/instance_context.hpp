#pragma once

// instance_context.hpp — Per-instance resource bundle.
//
// Each WASM instance (root or sub-instance) gets an InstanceContext
// that owns all its host-side resources: fd table, host API, database
// host, runtime host interfaces, strand, and the psizam instance itself.
//
// InstanceContext is created by InstancesHost::instantiate() for
// sub-instances, or by AppServer::runWorker() for the root instance.
// Destroying the context tears down all resources: closes fds, frees
// linear memory, releases the strand.

#include <psiserve/db_host.hpp>
#include <psiserve/host_api.hpp>
#include <psiserve/process.hpp>
#include <psiserve/runtime_host.hpp>
#include <psiserve/scheduler.hpp>

#include <psiber/strand.hpp>
#include <psizam/runtime.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace psiserve
{

   struct InstanceContext
   {
      psi::runtime::instance_id  id = 0;
      std::string                name;

      // The WASM instance (linear memory, backend, gas state)
      psizam::instance           inst;

      // Per-instance resources
      Process                    process;
      std::unique_ptr<HostApi>   host_api;
      std::unique_ptr<db_host>   db;

      // Scheduling context — serializes all fibers on this instance
      std::unique_ptr<psiber::strand> exec_strand;

      // Runtime host interfaces (identity, instances, dispatch)
      // Only set for trusted instances that have psi:runtime/* access.
      std::unique_ptr<RuntimeHost> runtime_host;

      // Parent context (null for root instances launched by config)
      InstanceContext* parent = nullptr;

      // Transfer a socket from this instance to a target instance.
      // Extracts the fd entry from this instance's fd table and
      // inserts it into the target's fd table.
      // Returns the new VirtualFd in the target, or invalid_virtual_fd.
      VirtualFd transfer_socket(VirtualFd source_vfd, InstanceContext& target)
      {
         auto entry = process.fds.extract(source_vfd);
         if (!entry)
            return invalid_virtual_fd;
         return target.process.fds.alloc(std::move(*entry));
      }
   };

}  // namespace psiserve
