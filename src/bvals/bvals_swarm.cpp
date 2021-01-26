//========================================================================================
// Athena++ astrophysical MHD code
// Copyright(C) 2014 James M. Stone <jmstone@princeton.edu> and other code contributors
// Licensed under the 3-clause BSD License, see LICENSE file for details
//========================================================================================
// (C) (or copyright) 2020. Triad National Security, LLC. All rights reserved.
//
// This program was produced under U.S. Government contract 89233218CNA000001 for Los
// Alamos National Laboratory (LANL), which is operated by Triad National Security, LLC
// for the U.S. Department of Energy/National Nuclear Security Administration. All rights
// in the program are reserved by Triad National Security, LLC, and the U.S. Department
// of Energy/National Nuclear Security Administration. The Government is granted for
// itself and others acting on its behalf a nonexclusive, paid-up, irrevocable worldwide
// license in this material to reproduce, prepare derivative works, distribute copies to
// the public, perform publicly and display publicly, and to permit others to do so.
//========================================================================================

//! \file bvals_swarm.cpp
//  \brief functions that apply BCs for SWARMs

#include "bvals/bvals_interfaces.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "basic_types.hpp"
#include "globals.hpp"
#include "mesh/mesh.hpp"
#include "parameter_input.hpp"
#include "utils/buffer_utils.hpp"
#include "utils/error_checking.hpp"

namespace parthenon {

BoundarySwarm::BoundarySwarm(std::weak_ptr<MeshBlock> pmb)
    : bswarm_index(), pmy_block(pmb), pmy_mesh_(pmb.lock()->pmy_mesh) {
#ifdef MPI_PARALLEL
  swarm_id_ = pmb.lock()->pbval->bvars_next_phys_id_;
#endif

  InitBoundaryData(bd_var_);
}

void BoundarySwarm::InitBoundaryData(BoundaryData<> &bd) {
  auto pmb = GetBlockPointer();
  NeighborIndexes *ni = pmb->pbval->ni;
  int size = 0;

  bd.nbmax = pmb->pbval->maxneighbor_;

  for (int n = 0; n < bd.nbmax; n++) {
    bd.flag[n] = BoundaryStatus::waiting;
#ifdef MPI_PARALLEL
    bd.req_send[n] = MPI_REQUEST_NULL;
    bd.req_recv[n] = MPI_REQUEST_NULL;
#endif
  }
}

void BoundarySwarm::SetupPersistentMPI() {
#ifdef MPI_PARALLEL
  std::shared_ptr<MeshBlock> pmb = GetBlockPointer();
  int &mylevel = pmb->loc.level;

  // Initialize neighbor communications to other ranks
  int tag;
  int ssize = 0;
  int rsize = 0;
  for (int n = 0; n < pmb->pbval->nneighbor; n++) {
    NeighborBlock &nb = pmb->pbval->neighbor[n];

    // Neighbor on different MPI process
    if (nb.snb.rank != Globals::my_rank) {
      send_tag[n] = pmb->pbval->CreateBvalsMPITag(nb.snb.lid, nb.targetid, swarm_id_);
      recv_tag[n] = pmb->pbval->CreateBvalsMPITag(pmb->lid, nb.bufid, swarm_id_);
      if (bd_var_.req_send[nb.bufid] != MPI_REQUEST_NULL) {
        MPI_Request_free(&bd_var_.req_send[nb.bufid]);
      }
      if (bd_var_.req_recv[nb.bufid] != MPI_REQUEST_NULL) {
        MPI_Request_free(&bd_var_.req_recv[nb.bufid]);
      }
    }
  }
#endif
}

// Send particle buffers across meshblocks. If different MPI ranks, use MPI, if same rank,
// do a deep copy on device.
void BoundarySwarm::Send(BoundaryCommSubset phase) {
  std::shared_ptr<MeshBlock> pmb = GetBlockPointer();
  int &mylevel = pmb->loc.level;
  // Fence to make sure buffers are loaded before sending
  pmb->exec_space.fence();
  for (int n = 0; n < pmb->pbval->nneighbor; n++) {
    NeighborBlock &nb = pmb->pbval->neighbor[n];
    if (nb.snb.rank != Globals::my_rank) {
#ifdef MPI_PARALLEL
      PARTHENON_REQUIRE(bd_var_.req_send[nb.bufid] == MPI_REQUEST_NULL,
                        "Trying to create a new send before previous send completes!");
      MPI_Isend(bd_var_.send[n].data(), send_size[n], MPI_PARTHENON_REAL, nb.snb.rank,
                send_tag[n], MPI_COMM_WORLD, &(bd_var_.req_send[nb.bufid]));
      if (send_size[n] > 0) {
        printf("[%i] SENDing %i particles (tag %i) to neighbor %i rank %i\n",
               Globals::my_rank, send_size[n] / particle_size, send_tag[n], n,
               nb.snb.rank);
      }
#endif // MPI_PARALLEL
    } else {
      MeshBlock &target_block = *pmy_mesh_->FindMeshBlock(nb.snb.gid);
      std::shared_ptr<BoundarySwarm> ptarget_bswarm =
          target_block.pbswarm->bswarms[bswarm_index];
      if (send_size[nb.bufid] > 0) {
        // Ensure target buffer is large enough
        if (bd_var_.send[nb.bufid].extent(0) >
            ptarget_bswarm->bd_var_.recv[nb.targetid].extent(0)) {
          ptarget_bswarm->bd_var_.recv[nb.targetid] =
              ParArray1D<Real>("Buffer", (bd_var_.send[nb.bufid].extent(0)));
        }

        target_block.deep_copy(ptarget_bswarm->bd_var_.recv[nb.targetid],
                               bd_var_.send[nb.bufid]);
        ptarget_bswarm->recv_size[nb.targetid] = send_size[nb.bufid];
        ptarget_bswarm->bd_var_.flag[nb.targetid] = BoundaryStatus::arrived;
        printf("[%i] COPYing %i particles to neighbor %i rank %i\n", Globals::my_rank,
               send_size[nb.bufid] / particle_size, n, nb.snb.rank);
      } else {
        ptarget_bswarm->recv_size[nb.targetid] = 0;
        ptarget_bswarm->bd_var_.flag[nb.targetid] = BoundaryStatus::completed;
      }
    }
  }
}

void BoundarySwarm::Receive(BoundaryCommSubset phase) {
#ifdef MPI_PARALLEL
  MPI_Barrier(MPI_COMM_WORLD);
  std::shared_ptr<MeshBlock> pmb = GetBlockPointer();
  int &mylevel = pmb->loc.level;
  for (int n = 0; n < pmb->pbval->nneighbor; n++) {
    NeighborBlock &nb = pmb->pbval->neighbor[n];
    if (nb.snb.rank != Globals::my_rank) {
      pmb->exec_space.fence();
      // Check to see if we got a message
      int test;
      MPI_Status status;

      if (bd_var_.flag[nb.bufid] != BoundaryStatus::completed) {
        MPI_Iprobe(MPI_ANY_SOURCE, recv_tag[nb.bufid], MPI_COMM_WORLD, &test, &status);
        printf("[%i] PROBEing tag %i: test: %i\n", Globals::my_rank, recv_tag[nb.bufid],
               test);
        if (!static_cast<bool>(test)) {
          bd_var_.flag[nb.bufid] = BoundaryStatus::waiting;
        } else {
          bd_var_.flag[nb.bufid] = BoundaryStatus::arrived;

          // If message is available, receive it
          int nbytes = 0;
          MPI_Get_count(&status, MPI_CHAR, &nbytes);
          if (nbytes / sizeof(Real) > bd_var_.recv[n].extent(0)) {
            bd_var_.recv[n] = ParArray1D<Real>("Buffer", nbytes / sizeof(Real));
          }
          MPI_Recv(bd_var_.recv[n].data(), nbytes, MPI_CHAR, nb.snb.rank,
                   recv_tag[nb.bufid], MPI_COMM_WORLD, &status);
          recv_size[n] = nbytes / sizeof(Real);
          printf("[%i] RECEIVEd %i particles from rank %i tag %i\n", Globals::my_rank,
                 nbytes / particle_size, nb.snb.rank, recv_tag[nb.bufid]);
        }
      }
    }
  }
  MPI_Barrier(MPI_COMM_WORLD);
#endif
}

} // namespace parthenon