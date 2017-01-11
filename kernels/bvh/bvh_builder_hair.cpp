// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "bvh_builder_hair.h"
#include "bvh_builder_hair_msmblur.h"
#include "../builders/primrefgen.h"

#include "../geometry/bezier1v.h"
#include "../geometry/bezier1i.h"

#if defined(EMBREE_GEOMETRY_HAIR)

namespace embree
{
  namespace isa
  {
    template<int N, typename Primitive>
    struct BVHNHairBuilderSAH : public Builder
    {
      typedef BVHN<N> BVH;
      typedef typename BVH::AlignedNode AlignedNode;
      typedef typename BVH::UnalignedNode UnalignedNode;
      typedef typename BVH::NodeRef NodeRef;
      typedef HeuristicArrayBinningSAH<BezierPrim,NUM_OBJECT_BINS> HeuristicBinningSAH;

      BVH* bvh;
      Scene* scene;
      mvector<BezierPrim> prims;

      BVHNHairBuilderSAH (BVH* bvh, Scene* scene)
        : bvh(bvh), scene(scene), prims(scene->device) {}
      
      void build(size_t, size_t) 
      {
        /* progress monitor */
        auto progress = [&] (size_t dn) { bvh->scene->progressMonitor(double(dn)); };
        auto virtualprogress = BuildProgressMonitorFromClosure(progress);

        /* fast path for empty BVH */
        const size_t numPrimitives = scene->getNumPrimitives<BezierCurves,false>();
        if (numPrimitives == 0) {
          prims.clear();
          bvh->set(BVH::emptyNode,empty,0);
          return;
        }

        double t0 = bvh->preBuild(TOSTRING(isa) "::BVH" + toString(N) + "BuilderHairSAH");

        //profile(1,5,numPrimitives,[&] (ProfileTimer& timer) {
        
        /* create primref array */
        bvh->alloc.init_estimate(numPrimitives*sizeof(Primitive));
        prims.resize(numPrimitives);
        const PrimInfo pinfo = createBezierRefArray(scene,prims,virtualprogress);
        
        /* build hierarchy */
        typename BVH::NodeRef root = bvh_obb_builder_binned_sah<N>
          (
            [&] () { return bvh->alloc.threadLocal2(); },

            [&] (const PrimInfo* children, const size_t numChildren, 
                 HeuristicBinningSAH alignedHeuristic, 
                 FastAllocator::ThreadLocal2* alloc) -> AlignedNode*
            {
              AlignedNode* node = (AlignedNode*) alloc->alloc0->malloc(sizeof(AlignedNode),BVH::byteNodeAlignment); node->clear();
              for (size_t i=0; i<numChildren; i++)
                node->set(i,children[i].geomBounds);
              return node;
            },
            
            [&] (const PrimInfo* children, const size_t numChildren, 
                 UnalignedHeuristicArrayBinningSAH<BezierPrim,NUM_OBJECT_BINS> unalignedHeuristic, 
                 FastAllocator::ThreadLocal2* alloc) -> UnalignedNode*
            {
              UnalignedNode* node = (UnalignedNode*) alloc->alloc0->malloc(sizeof(UnalignedNode),BVH::byteNodeAlignment); node->clear();
              for (size_t i=0; i<numChildren; i++) 
              {
                const LinearSpace3fa space = unalignedHeuristic.computeAlignedSpace(children[i]); 
                const PrimInfo       sinfo = unalignedHeuristic.computePrimInfo(children[i],space);
                node->set(i,OBBox3fa(space,sinfo.geomBounds));
              }
              return node;
            },

            [&] (size_t depth, const PrimInfo& pinfo, FastAllocator::ThreadLocal2* alloc) -> NodeRef
            {
              size_t items = pinfo.size();
              size_t start = pinfo.begin;
              Primitive* accel = (Primitive*) alloc->alloc1->malloc(items*sizeof(Primitive));
              NodeRef node = bvh->encodeLeaf((char*)accel,items);
              for (size_t i=0; i<items; i++) {
                accel[i].fill(prims.data(),start,pinfo.end,bvh->scene);
              }
              return node;
            },
            progress,
            prims.data(),pinfo,N,BVH::maxBuildDepthLeaf,1,1,BVH::maxLeafBlocks);
        
        bvh->set(root,LBBox3fa(pinfo.geomBounds),pinfo.size());
        
        //});
        
        /* clear temporary data for static geometry */
        if (scene->isStatic()) {
          prims.clear();
          bvh->shrink();
        }
        bvh->cleanup();
        bvh->postBuild(t0);
      }

      void clear() {
        prims.clear();
      }
    };


    template<int N, typename Primitive>
    struct BVHNHairMBBuilderSAH : public Builder
    {
      typedef BVHN<N> BVH;
      typedef typename BVH::AlignedNodeMB AlignedNodeMB;
      typedef typename BVH::UnalignedNodeMB UnalignedNodeMB;
      typedef typename BVH::NodeRef NodeRef;
      typedef HeuristicArrayBinningSAH<BezierPrim,NUM_OBJECT_BINS> HeuristicBinningSAH;

      BVH* bvh;
      Scene* scene;
      mvector<BezierPrim> prims;

      BVHNHairMBBuilderSAH (BVH* bvh, Scene* scene)
        : bvh(bvh), scene(scene), prims(scene->device) {}
      
      void build(size_t, size_t) 
      {
        /* progress monitor */
        auto progress = [&] (size_t dn) { bvh->scene->progressMonitor(double(dn)); };
        auto virtualprogress = BuildProgressMonitorFromClosure(progress);

        /* fast path for empty BVH */
        const size_t numPrimitives = scene->getNumPrimitives<BezierCurves,true>();
        if (numPrimitives == 0) {
          prims.clear();
          bvh->set(BVH::emptyNode,empty,0);
          return;
        }
        
        double t0 = bvh->preBuild(TOSTRING(isa) "::BVH" + toString(N) + "BuilderMBHairSAH");

        //profile(1,5,numPrimitives,[&] (ProfileTimer& timer) {

        /* create primref array */
        bvh->numTimeSteps = scene->getNumTimeSteps<BezierCurves,true>();
        const size_t numTimeSegments = bvh->numTimeSteps-1; assert(bvh->numTimeSteps > 1);
        prims.resize(numPrimitives);
        bvh->alloc.init_estimate(numPrimitives*sizeof(Primitive)*numTimeSegments);
        NodeRef* roots = (NodeRef*) bvh->alloc.threadLocal2()->alloc0->malloc(sizeof(NodeRef)*numTimeSegments,BVH::byteNodeAlignment);

        /* build BVH for each timestep */
        avector<BBox3fa> bounds(bvh->numTimeSteps);
        size_t num_bvh_primitives = 0;
        for (size_t t=0; t<numTimeSegments; t++)
        {
          /* call BVH builder */
          const PrimInfo pinfo = createBezierRefArrayMBlur(t,bvh->numTimeSteps,scene,prims,virtualprogress);
          const LBBox3fa lbbox = HeuristicBinningSAH(prims.begin()).computePrimInfoMB(t,bvh->numTimeSteps,scene,pinfo);
        
          NodeRef root = bvh_obb_builder_binned_sah<N>
          (
            [&] () { return bvh->alloc.threadLocal2(); },

            [&] (const PrimInfo* children, const size_t numChildren, HeuristicBinningSAH alignedHeuristic, FastAllocator::ThreadLocal2* alloc) -> AlignedNodeMB*
            {
              AlignedNodeMB* node = (AlignedNodeMB*) alloc->alloc0->malloc(sizeof(AlignedNodeMB),BVH::byteNodeAlignment); node->clear();
              for (size_t i=0; i<numChildren; i++) 
              {
                LBBox3fa bounds = alignedHeuristic.computePrimInfoMB(t,bvh->numTimeSteps,scene,children[i]);
                node->set(i,bounds);
              }
              return node;
            },
            
            [&] (const PrimInfo* children, const size_t numChildren, UnalignedHeuristicArrayBinningSAH<BezierPrim,NUM_OBJECT_BINS> unalignedHeuristic, FastAllocator::ThreadLocal2* alloc) -> UnalignedNodeMB*
            {
              UnalignedNodeMB* node = (UnalignedNodeMB*) alloc->alloc0->malloc(sizeof(UnalignedNodeMB),BVH::byteNodeAlignment); node->clear();
              for (size_t i=0; i<numChildren; i++) 
              {
                const AffineSpace3fa space = unalignedHeuristic.computeAlignedSpaceMB(scene,children[i]); 
                UnalignedHeuristicArrayBinningSAH<BezierPrim,NUM_OBJECT_BINS>::PrimInfoMB pinfo = unalignedHeuristic.computePrimInfoMB(t,bvh->numTimeSteps,scene,children[i],space);
                node->set(i,space,pinfo.s0t0,pinfo.s1t1); // FIXME: do we have to globalize these bounds?
              }
              return node;
            },

            [&] (size_t depth, const PrimInfo& pinfo, FastAllocator::ThreadLocal2* alloc) -> NodeRef
            {
              size_t items = pinfo.size();
              size_t start = pinfo.begin;
              Primitive* accel = (Primitive*) alloc->alloc1->malloc(items*sizeof(Primitive));
              NodeRef node = bvh->encodeLeaf((char*)accel,items);
              for (size_t i=0; i<items; i++) {
                accel[i].fill(prims.data(),start,pinfo.end,bvh->scene);
              }
              return node;
            },
            progress,
            prims.data(),pinfo,N,BVH::maxBuildDepthLeaf,1,1,BVH::maxLeafBlocks);

          roots[t] = root;
          bounds[t+0] = lbbox.bounds0;
          bounds[t+1] = lbbox.bounds1;
          num_bvh_primitives = max(num_bvh_primitives,pinfo.size());
        }
        bvh->set(NodeRef((size_t)roots),LBBox3fa(bounds),num_bvh_primitives);
        bvh->msmblur = true;

        //});
        
        /* clear temporary data for static geometry */
        if (scene->isStatic()) {
          prims.clear();
          bvh->shrink();
        }
        bvh->cleanup();
        bvh->postBuild(t0);
      }

      void clear() {
        prims.clear();
      }
    };

#if 0
    template<int N, typename Primitive>
    struct BVHNHairMSMBlurBuilderSAH : public Builder
    {
      typedef BVHN<N> BVH;
      typedef typename BVH::AlignedNodeMB AlignedNodeMB;
      typedef typename BVH::AlignedNodeMB4D AlignedNodeMB4D;
      typedef typename BVH::UnalignedNodeMB UnalignedNodeMB;
      typedef typename BVH::NodeRef NodeRef;
      typedef HeuristicArrayBinningMB<PrimRefMB,NUM_OBJECT_BINS> HeuristicBinning;
      typedef UnalignedHeuristicArrayBinningMB<PrimRefMB,NUM_OBJECT_BINS> UnalignedHeuristicBinning;

      BVH* bvh;
      Scene* scene;
      mvector<PrimRefMB> prims;

      BVHNHairMSMBlurBuilderSAH (BVH* bvh, Scene* scene)
        : bvh(bvh), scene(scene), prims(scene->device) {}
      
      void build(size_t, size_t) 
      {
        /* progress monitor */
        auto progress = [&] (size_t dn) { bvh->scene->progressMonitor(double(dn)); };
        auto virtualprogress = BuildProgressMonitorFromClosure(progress);

        /* fast path for empty BVH */
        const size_t numPrimitives = scene->getNumPrimitives<BezierCurves,true>();
        if (numPrimitives == 0) {
          prims.clear();
          bvh->set(BVH::emptyNode,empty,0);
          return;
        }

        double t0 = bvh->preBuild(TOSTRING(isa) "::BVH" + toString(N) + "BuilderMSMBlurHairSAH");

        //profile(1,5,numPrimitives,[&] (ProfileTimer& timer) {
        
        /* create primref array */
        bvh->alloc.init_estimate(numPrimitives*sizeof(Primitive));
        prims.resize(numPrimitives);
        const PrimInfoMB pinfo = createPrimRefMBArray<BezierCurves>(scene,prims,virtualprogress);

        RecalculatePrimRef<BezierCurves> recalculatePrimRef(scene);

        SetMB set(&prims,range<size_t>(0,pinfo.size()),BBox1f(0.0f,1.0f));
        BuildRecord2 record(pinfo,set,0);
        
        /* build hierarchy */
        typename BVH::NodeRef root = bvh_obb_builder_binned_sah_mblur<N>
          (
            scene,
            recalculatePrimRef,
            
            [&] () { return bvh->alloc.threadLocal2(); },
            
            [&] (const BuildRecord2* children, const size_t numChildren, 
                 HeuristicBinning alignedHeuristic, 
                 FastAllocator::ThreadLocal2* alloc) -> AlignedNodeMB*
            {
              AlignedNodeMB* node = (AlignedNodeMB*) alloc->alloc0->malloc(sizeof(AlignedNodeMB),BVH::byteNodeAlignment); node->clear();
              for (size_t i=0; i<numChildren; i++) {
                LBBox3fa cbounds = children[i].prims.linearBounds(recalculatePrimRef);
                BBox1f dt = children[i].prims.time_range;
                node->set(i,cbounds.global(dt));
              }
              return node;
            },
            
            [&] (const BuildRecord2* children, const size_t numChildren, 
                 UnalignedHeuristicBinning unalignedHeuristic, 
                 FastAllocator::ThreadLocal2* alloc) -> UnalignedNodeMB*
            {
              UnalignedNodeMB* node = (UnalignedNodeMB*) alloc->alloc0->malloc(sizeof(UnalignedNodeMB),BVH::byteNodeAlignment); node->clear();
              for (size_t i=0; i<numChildren; i++) 
              {
                const LinearSpace3fa space = unalignedHeuristic.computeAlignedSpaceMB(scene,children[i].prims); 
                LBBox3fa lbounds = unalignedHeuristic.linearBounds(scene,children[i].prims,space);
                node->set(i,space,lbounds.global(children[i].prims.time_range));
              }
              return node;
            },

            [&] (const BuildRecord2* children, const size_t numChildren, FastAllocator::ThreadLocal2* alloc) -> AlignedNodeMB4D*
            {
              AlignedNodeMB4D* node = (AlignedNodeMB4D*) alloc->alloc0->malloc(sizeof(AlignedNodeMB4D),BVH::byteNodeAlignment); node->clear();
              for (size_t i=0; i<numChildren; i++) {
                LBBox3fa cbounds = children[i].prims.linearBounds(recalculatePrimRef);
                BBox1f dt = children[i].prims.time_range;
                node->set(i,cbounds.global(dt),dt);
              }
              return node;
            },

            [&] (const BuildRecord2& current, FastAllocator::ThreadLocal2* alloc) -> NodeRef
            {
              size_t items = current.prims.object_range.size();
              size_t start = current.prims.object_range.begin();
              Primitive* accel = (Primitive*) alloc->alloc1->malloc(items*sizeof(Primitive));
              NodeRef node = bvh->encodeLeaf((char*)accel,items);
              for (size_t i=0; i<items; i++) {
                accel[i].fill(prims.data(),start,current.prims.object_range.end(),bvh->scene);
              }
              return node;
            },
            progress,
            record,N,BVH::maxBuildDepthLeaf,1,1,BVH::maxLeafBlocks);
        
        bvh->set(root,LBBox3fa(pinfo.geomBounds),pinfo.size());
        
        //});
        
        /* clear temporary data for static geometry */
        if (scene->isStatic()) {
          prims.clear();
          bvh->shrink();
        }
        bvh->cleanup();
        bvh->postBuild(t0);
      }

      void clear() {
        prims.clear();
      }
    };

     Builder* BVH4Bezier1iMSMBlurBuilder_OBB (void* bvh, Scene* scene, size_t mode) { return new BVHNHairMSMBlurBuilderSAH<4,Bezier1i>((BVH4*)bvh,scene); }

#endif
    
    /*! entry functions for the builder */
    Builder* BVH4Bezier1vBuilder_OBB_New   (void* bvh, Scene* scene, size_t mode) { return new BVHNHairBuilderSAH<4,Bezier1v>((BVH4*)bvh,scene); }
    Builder* BVH4Bezier1iBuilder_OBB_New   (void* bvh, Scene* scene, size_t mode) { return new BVHNHairBuilderSAH<4,Bezier1i>((BVH4*)bvh,scene); }
    Builder* BVH4Bezier1iMBBuilder_OBB_New (void* bvh, Scene* scene, size_t mode) { return new BVHNHairMBBuilderSAH<4,Bezier1i>((BVH4*)bvh,scene); }

#if defined(__AVX__)
    Builder* BVH8Bezier1vBuilder_OBB_New   (void* bvh, Scene* scene, size_t mode) { return new BVHNHairBuilderSAH<8,Bezier1v>((BVH8*)bvh,scene); }
    Builder* BVH8Bezier1iBuilder_OBB_New   (void* bvh, Scene* scene, size_t mode) { return new BVHNHairBuilderSAH<8,Bezier1i>((BVH8*)bvh,scene); }
    Builder* BVH8Bezier1iMBBuilder_OBB_New (void* bvh, Scene* scene, size_t mode) { return new BVHNHairMBBuilderSAH<8,Bezier1i>((BVH8*)bvh,scene); }
#endif

  }
}
#endif
