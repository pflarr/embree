// ======================================================================== //
// Copyright 2009-2016 Intel Corporation                                    //
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

#pragma once

#include "../common/primref_mb.h"
#include "heuristic_binning.h"

#define MBLUR_SPLIT_OVERLAP_THRESHOLD 0.1f
#define MBLUR_TIME_SPLIT_THRESHOLD 1.10f
#define MBLUR_TIME_SPLIT_LOCATIONS 1
#define MBLUR_NEW_ARRAY 0

namespace embree
{
  namespace isa
  { 
    /*! Performs standard object binning */
    template<typename Mesh, size_t BINS>
      struct HeuristicMBlur
      {
        typedef BinSplit<BINS> Split;
        typedef BinSplit<BINS> ObjectSplit;
        typedef BinSplit<BINS> TemporalSplit;
        typedef BinInfoT<BINS,PrimRefMB,LBBox3fa> ObjectBinner;
#if MBLUR_NEW_ARRAY
        typedef std::shared_ptr<avector<PrimRefMB>> PrimRefVector;
#else
        typedef avector<PrimRefMB>* PrimRefVector;
#endif

        struct Set 
        {
          __forceinline Set () {}

          __forceinline Set(PrimRefVector prims, range<size_t> object_range, BBox1f time_range)
            : prims(prims), object_range(object_range), time_range(time_range) {}

          __forceinline Set(PrimRefVector prims, BBox1f time_range = BBox1f(0.0f,1.0f))
            : prims(prims), object_range(range<size_t>(0,prims->size())), time_range(time_range) {}

        public:
          PrimRefVector prims;
          range<size_t> object_range;
          BBox1f time_range;
        };

        static const size_t PARALLEL_THRESHOLD = 3 * 1024;
        static const size_t PARALLEL_FIND_BLOCK_SIZE = 1024;
        static const size_t PARALLEL_PARITION_BLOCK_SIZE = 128;

        HeuristicMBlur (Scene* scene)
          : scene(scene) {}

        static __forceinline unsigned calculateNumOverlappingTimeSegments(Scene* scene, unsigned geomID, BBox1f time_range)
        {
          const unsigned totalTimeSegments = scene->get(geomID)->numTimeSegments();
          const unsigned itime_lower = floor(1.0001f*time_range.lower*float(totalTimeSegments));
          const unsigned itime_upper = ceil (0.9999f*time_range.upper*float(totalTimeSegments));
          const unsigned numTimeSegments = itime_upper-itime_lower; 
          assert(numTimeSegments > 0);
          return numTimeSegments;
        }

        /*! finds the best split */
        const Split find(Set& set, PrimInfoMB& pinfo, const size_t logBlockSize)
        {
          /* first try standard object split */
          const ObjectSplit object_split = object_find(set,pinfo,logBlockSize);
          const float object_split_sah = object_split.splitSAH();

          /* calculate number of timesegments */
          unsigned numTimeSegments = 0;
          for (size_t i=set.object_range.begin(); i<set.object_range.end(); i++) {
            const PrimRefMB& prim = (*set.prims)[i];
            unsigned segments = scene->get(prim.geomID())->numTimeSegments();
            numTimeSegments = max(numTimeSegments,segments);
          }
  
          /* do temporal splits only if the child bounds overlap */
          //const BBox3fa overlap = intersect(oinfo.leftBounds, oinfo.rightBounds);
          //if (safeArea(overlap) >= MBLUR_SPLIT_OVERLAP_THRESHOLD*safeArea(pinfo.geomBounds))
          if (set.time_range.size() > 1.99f/float(numTimeSegments))
          //if (set.time_range.size() > 1.01f/float(numTimeSegments))
          {
            TemporalSplit temporal_split = temporal_find(set, pinfo, logBlockSize, numTimeSegments);
            const float temporal_split_sah = temporal_split.splitSAH();

            //if (set.time_range.size() > 1.01f/float(time_segments))
            //  return temporal_split;

            /*float travCost = 1.0f;
            float intCost = 1.0f;
            float bestSAH = min(temporal_split_sah,object_split_sah);
            if (intCost*pinfo.leafSAH(logBlockSize) < travCost*expectedApproxHalfArea(pinfo.geomBounds)+intCost*bestSAH)
            {
              temporal_split.sah = float(neg_inf);
              return temporal_split;
              }*/

            /* force time split if object partitioning was not very successfull */
            /*float leafSAH = pinfo.leafSAH(logBlockSize);
            if (object_split_sah > 0.7f*leafSAH) {
              temporal_split.sah = float(neg_inf);
              return temporal_split;
              }*/

            /* take temporal split if it improved SAH */
            if (temporal_split_sah < object_split_sah)
              return temporal_split;
          }

          return object_split;
        }

        /*! finds the best split */
        const ObjectSplit object_find(const Set& set, const PrimInfoMB& pinfo, const size_t logBlockSize)
        {
          ObjectBinner binner(empty); // FIXME: this clear can be optimized away
          const BinMapping<BINS> mapping(pinfo.centBounds,pinfo.size());
          binner.bin_parallel(set.prims->data(),set.object_range.begin(),set.object_range.end(),PARALLEL_FIND_BLOCK_SIZE,PARALLEL_THRESHOLD,mapping);
          ObjectSplit osplit = binner.best(mapping,logBlockSize);
          osplit.sah *= pinfo.time_range.size();
          return osplit;
        }

        template<int LOCATIONS>
        struct TemporalBinInfo
        {
          __forceinline TemporalBinInfo () {
          }
          
          __forceinline TemporalBinInfo (EmptyTy)
          {
            for (size_t i=0; i<LOCATIONS; i++)
            {
              count0[i] = count1[i] = 0;
              bounds0[i] = bounds1[i] = empty;
            }
          }
          
          void bin(const PrimRefMB* prims, size_t begin, size_t end, BBox1f time_range, size_t numTimeSegments, Scene* scene)
          {
            for (int b=0; b<MBLUR_TIME_SPLIT_LOCATIONS; b++)
            {
              float t = float(b+1)/float(MBLUR_TIME_SPLIT_LOCATIONS+1);
              float ct = lerp(time_range.lower,time_range.upper,t);
              const float center_time = round(ct * float(numTimeSegments)) / float(numTimeSegments);
              if (center_time <= time_range.lower) continue;
              if (center_time >= time_range.upper) continue;
              const BBox1f dt0(time_range.lower,center_time);
              const BBox1f dt1(center_time,time_range.upper);
              
              /* find linear bounds for both time segments */
              for (size_t i=begin; i<end; i++) 
              {
                const unsigned geomID = prims[i].geomID();
                const unsigned primID = prims[i].primID();
                bounds0[b].extend(((Mesh*)scene->get(geomID))->linearBounds(primID,dt0));
                bounds1[b].extend(((Mesh*)scene->get(geomID))->linearBounds(primID,dt1));
                count0[b] += calculateNumOverlappingTimeSegments(scene,geomID,dt0);
                count1[b] += calculateNumOverlappingTimeSegments(scene,geomID,dt1);
              }
            }
          }

          __forceinline void bin_parallel(const PrimRefMB* prims, size_t begin, size_t end, size_t blockSize, size_t parallelThreshold, BBox1f time_range, size_t numTimeSegments, Scene* scene) 
          {
            if (likely(end-begin < parallelThreshold)) {
              bin(prims,begin,end,time_range,numTimeSegments,scene);
            } else {
              TemporalBinInfo binner(empty);
              *this = parallel_reduce(begin,end,blockSize,binner,
                                      [&](const range<size_t>& r) -> TemporalBinInfo { TemporalBinInfo binner(empty); binner.bin(prims, r.begin(), r.end(), time_range, numTimeSegments, scene); return binner; },
                                      [&](const TemporalBinInfo& b0, const TemporalBinInfo& b1) -> TemporalBinInfo { TemporalBinInfo r = b0; r.merge(b1); return r; });
            }
          }
          
          /*! merges in other binning information */
          __forceinline void merge (const TemporalBinInfo& other)
          {
            for (size_t i=0; i<LOCATIONS; i++) 
            {
              count0[i] += other.count0[i];
              count1[i] += other.count1[i];
              bounds0[i].extend(other.bounds0[i]);
              bounds1[i].extend(other.bounds1[i]);
            }
          }
          
          TemporalSplit best(int logBlockSize, BBox1f time_range, size_t numTimeSegments)
          {
            float bestSAH = inf;
            float bestPos = 0.0f;
            for (int b=0; b<MBLUR_TIME_SPLIT_LOCATIONS; b++)
            {
              float t = float(b+1)/float(MBLUR_TIME_SPLIT_LOCATIONS+1);
              float ct = lerp(time_range.lower,time_range.upper,t);
              const float center_time = round(ct * float(numTimeSegments)) / float(numTimeSegments);
              if (center_time <= time_range.lower) continue;
              if (center_time >= time_range.upper) continue;
              const BBox1f dt0(time_range.lower,center_time);
              const BBox1f dt1(center_time,time_range.upper);
              
              /* calculate sah */
              const size_t lCount = (count0[b]+(1 << logBlockSize)-1) >> int(logBlockSize);
              const size_t rCount = (count1[b]+(1 << logBlockSize)-1) >> int(logBlockSize);
              const float sah0 = bounds0[b].expectedApproxHalfArea()*float(lCount)*dt0.size();
              const float sah1 = bounds1[b].expectedApproxHalfArea()*float(rCount)*dt1.size();
              const float sah = sah0+sah1;
              if (sah < bestSAH) {
                bestSAH = sah;
                bestPos = center_time;
              }
            }
            assert(bestSAH != float(inf));
            return TemporalSplit(bestSAH*MBLUR_TIME_SPLIT_THRESHOLD,-1,0,bestPos);
          }
          
        public:
          size_t count0[LOCATIONS];
          size_t count1[LOCATIONS];
          LBBox3fa bounds0[LOCATIONS];
          LBBox3fa bounds1[LOCATIONS];
        };
        
        /*! finds the best split */
        const TemporalSplit temporal_find(const Set& set, const PrimInfoMB& pinfo, const size_t logBlockSize, const unsigned numTimeSegments)
        {
          assert(set.object_range.size() > 0);
          TemporalBinInfo<MBLUR_TIME_SPLIT_LOCATIONS> binner(empty);
          binner.bin_parallel(set.prims->data(),set.object_range.begin(),set.object_range.end(),PARALLEL_FIND_BLOCK_SIZE,PARALLEL_THRESHOLD,set.time_range,numTimeSegments,scene);
          return binner.best(logBlockSize,set.time_range,numTimeSegments);
        }
        
        /*! array partitioning */
        void split(const Split& split, const PrimInfoMB& pinfo, const Set& set, PrimInfoMB& left, Set& lset, PrimInfoMB& right, Set& rset)
        {
          /* valid split */
          if (unlikely(!split.valid())) {
            deterministic_order(set);
            return splitFallback(set,left,lset,right,rset);
          }

          /* perform temporal split */
          if (unlikely(split.data != 0))
            temporal_split(split,pinfo,set,left,lset,right,rset);
          
          /* perform object split */
          else 
            object_split(split,pinfo,set,left,lset,right,rset);
        }

        /*! array partitioning */
        __forceinline void object_split(const ObjectSplit& split, const PrimInfoMB& pinfo, const Set& set, PrimInfoMB& left, Set& lset, PrimInfoMB& right, Set& rset)
        {
          const size_t begin = set.object_range.begin();
          const size_t end   = set.object_range.end();
          left = empty;
          right = empty;
          const vint4 vSplitPos(split.pos);
          const vbool4 vSplitMask(1 << split.dim);
          auto isLeft = [&] (const PrimRefMB &ref) { return any(((vint4)split.mapping.bin_unsafe(ref) < vSplitPos) & vSplitMask); };
          auto reduction = [] (PrimInfoMB& pinfo, const PrimRefMB& ref) { pinfo.add_primref(ref); };
          auto reduction2 = [] (PrimInfoMB& pinfo0,const PrimInfoMB& pinfo1) { pinfo0.merge(pinfo1); };
          size_t center = parallel_partitioning(set.prims->data(),begin,end,empty,left,right,isLeft,reduction,reduction2,PARALLEL_PARITION_BLOCK_SIZE,PARALLEL_THRESHOLD);
          left.begin  = begin;  left.end = center; left.time_range = pinfo.time_range;
          right.begin = center; right.end = end;   right.time_range = pinfo.time_range;
          new (&lset) Set(set.prims,range<size_t>(begin,center),set.time_range);
          new (&rset) Set(set.prims,range<size_t>(center,end  ),set.time_range);
        }

        /*! array partitioning */
        __forceinline void temporal_split(const TemporalSplit& split, const PrimInfoMB& pinfo, const Set& set, PrimInfoMB& linfo, Set& lset, int side)
        {
          float center_time = split.fpos;
          const BBox1f time_range0(set.time_range.lower,center_time);
          const BBox1f time_range1(center_time,set.time_range.upper);
          const BBox1f time_range = side ? time_range1 : time_range0;
          
          /* calculate primrefs for first time range */
#if MBLUR_NEW_ARRAY
          std::shared_ptr<avector<PrimRefMB>> lprims(new avector<PrimRefMB>(set.object_range.size()));
#endif
          auto reduction_func0 = [&] ( const range<size_t>& r) {
            PrimInfoMB pinfo = empty;
            for (size_t i=r.begin(); i<r.end(); i++) 
            {
              avector<PrimRefMB>& prims = *set.prims;
              const unsigned geomID = prims[i].geomID();
              const unsigned primID = prims[i].primID();
              const LBBox3fa lbounds = ((Mesh*)scene->get(geomID))->linearBounds(primID,time_range);
              const unsigned num_time_segments = calculateNumOverlappingTimeSegments(scene,geomID,time_range);
              const PrimRefMB prim(lbounds,num_time_segments,geomID,primID);
#if MBLUR_NEW_ARRAY
              (*lprims)[i-set.object_range.begin()] = prim;
#else
              prims[i] = prim;
#endif
              pinfo.add_primref(prim);
            }
            return pinfo;
          };        
          linfo = parallel_reduce(set.object_range.begin(),set.object_range.end(),PARALLEL_PARITION_BLOCK_SIZE,PARALLEL_THRESHOLD,PrimInfoMB(empty),reduction_func0,
                                  [] (const PrimInfoMB& a, const PrimInfoMB& b) { return PrimInfoMB::merge(a,b); });
   
          linfo.time_range = time_range;
#if MBLUR_NEW_ARRAY
          lset = Set(lprims,time_range);
#else
          lset = Set(set.prims,set.object_range,time_range);
          linfo.begin = lset.object_range.begin(); linfo.end = lset.object_range.end();
#endif
        }

        __forceinline void temporal_split(const TemporalSplit& split, const PrimInfoMB& pinfo, const Set& set, PrimInfoMB& linfo, Set& lset, PrimInfoMB& rinfo, Set& rset)
        {
          temporal_split(split,pinfo,set,linfo,lset,0);
          temporal_split(split,pinfo,set,rinfo,rset,1);
        }

        void deterministic_order(const Set& set) 
        {
          /* required as parallel partition destroys original primitive order */
          PrimRefMB* prims = set.prims->data();
          std::sort(&prims[set.object_range.begin()],&prims[set.object_range.end()]);
        }

        void splitFallback(const Set& set, PrimInfoMB& linfo, Set& lset, PrimInfoMB& rinfo, Set& rset) // FIXME: also perform time split here?
        {
          avector<PrimRefMB>& prims = *set.prims;

          const size_t begin = set.object_range.begin();
          const size_t end   = set.object_range.end();
          const size_t center = (begin + end)/2;
          
          linfo = empty;
          for (size_t i=begin; i<center; i++)
            linfo.add_primref(prims[i]);
          linfo.begin = begin; linfo.end = center; linfo.time_range = set.time_range;
          
          rinfo = empty;
          for (size_t i=center; i<end; i++)
            rinfo.add_primref(prims[i]);	
          rinfo.begin = center; rinfo.end = end; rinfo.time_range = set.time_range;
          
          new (&lset) Set(set.prims,range<size_t>(begin,center),set.time_range);
          new (&rset) Set(set.prims,range<size_t>(center,end  ),set.time_range);
        }

      private:
        Scene* scene;
      };

    template<typename BuildRecord, 
      typename Mesh, 
      typename ReductionTy, 
      typename Allocator, 
      typename CreateAllocFunc, 
      typename CreateNodeFunc, 
      typename UpdateNodeFunc, 
      typename CreateLeafFunc, 
      typename ProgressMonitor,
      typename PrimInfo>
      
      class GeneralBVHMBBuilder : public HeuristicMBlur<Mesh,NUM_OBJECT_BINS>
      {
        static const size_t MAX_BRANCHING_FACTOR = 8;        //!< maximal supported BVH branching factor
        static const size_t MIN_LARGE_LEAF_LEVELS = 8;        //!< create balanced tree of we are that many levels before the maximal tree depth
        static const size_t SINGLE_THREADED_THRESHOLD = 1024;  //!< threshold to switch to single threaded build
        typedef HeuristicMBlur<Mesh,NUM_OBJECT_BINS> Heuristic;
        
      public:

        struct LocalTree
        {
          struct Node
          {
            __forceinline Node () {}
            __forceinline Node (BuildRecord& record, Node* parent = nullptr, bool right = false, bool valid = true)
              : record(record), valid(valid), lchild(nullptr), rchild(nullptr), parent(parent)
            {
              if (parent) {
                if (right) parent->rchild = this;
                else       parent->lchild = this;
              }
            }

          public:
            BuildRecord record;
            bool valid;
            Node* lchild;
            Node* rchild;
            Node* parent;
          };

          __forceinline LocalTree (GeneralBVHMBBuilder* builder, BuildRecord& record)
            : builder(builder), numNodes(0), numChildren(0), depth(record.depth), hasTimeSplit(false)
          {
            children[numChildren++] = add(record);
          }

          __forceinline Node* add(BuildRecord& record, Node* parent = nullptr, bool right = false, bool valid = true) {
            return new (&nodes[numNodes++]) Node(record,parent,right,valid);
          }

          __forceinline void split(size_t bestChild)
          {
            restore(bestChild);

            Node* node = children[bestChild];
            BuildRecord& brecord = node->record;
            BuildRecord lrecord(depth+1);
            BuildRecord rrecord(depth+1);

            /* temporal split */
            if (brecord.split.data == -1 && brecord.split.valid())
            {
              hasTimeSplit = true;
              builder->temporal_split(brecord.split,brecord.pinfo,brecord.prims,lrecord.pinfo,lrecord.prims,false);
              lrecord.split = builder->find(lrecord);
              builder->temporal_split(brecord.split,brecord.pinfo,brecord.prims,rrecord.pinfo,rrecord.prims,true);
              rrecord.split = builder->find(rrecord);
              children[bestChild  ] = add(lrecord,node,false,false);
              children[numChildren] = add(rrecord,node,true ,true );
              numChildren++;
            } 
            /* object split */
            else 
            {
              builder->partition(brecord,lrecord,rrecord);
              lrecord.split = builder->find(lrecord);
              rrecord.split = builder->find(rrecord);
              children[bestChild  ] = add(lrecord,node,false);
              children[numChildren] = add(rrecord,node,true);
              numChildren++;
            }
          }

          __forceinline size_t size() const {
            return numChildren;
          }

          __forceinline BuildRecord& operator[] ( size_t i) {
            return children[i]->record;
          }
          
          bool hasTimeSplits() const {
            return hasTimeSplit;
          }

          bool restore(Node* node, Node* child)
          {
            /* first restore all prior splits */
            bool invalid = !child->valid;
            assert(node->lchild == child || node->rchild == child);
            if (node->parent) invalid |= restore(node->parent,node);
            
            /* if the node we came from was invalid and this is a time split node then we have to recalculate the invalid node */
            if (invalid)
            {
              if (node->record.split.data == -1) {
                const bool right = node->rchild == child;
                builder->temporal_split(node->record.split,node->record.pinfo,node->record.prims,child->record.pinfo,child->record.prims,right);
                node->lchild->valid = !right;
                node->rchild->valid = right;
              } else {
                builder->partition(node->record,node->lchild->record,node->rchild->record);
              }
            }
            return invalid;
          }
          
          __forceinline void restore(size_t childID) 
          {
            if (children[childID]->parent)
              restore(children[childID]->parent,children[childID]);
          }

          __forceinline ssize_t best()
          {
            /*! find best child to split */
            float bestSAH = neg_inf;
            ssize_t bestChild = -1;
            for (size_t i=0; i<numChildren; i++) 
            {
              if (children[i]->record.pinfo.size() <= builder->minLeafSize) continue; 
              if (expectedApproxHalfArea(children[i]->record.pinfo.geomBounds) > bestSAH) {
                bestChild = i; bestSAH = expectedApproxHalfArea(children[i]->record.pinfo.geomBounds); 
              } 
            }
            return bestChild;
          }

        private:
          GeneralBVHMBBuilder* builder;
          Node nodes[2*MAX_BRANCHING_FACTOR];
          size_t numNodes;
          Node* children[MAX_BRANCHING_FACTOR];
          size_t numChildren;
          size_t depth;
          bool hasTimeSplit;
        };

        struct LocalChildList
        {
          __forceinline LocalChildList (GeneralBVHMBBuilder* builder, BuildRecord& record)
            : builder(builder), numChildren(0), depth(record.depth) 
          {
            add(record);
          }

          __forceinline void add(BuildRecord& record) {
            children[numChildren] = record;
            active[numChildren] = true;
            numChildren++;
          }

          __forceinline void split(size_t bestChild)
          {
            /* perform best found split */
            BuildRecord& brecord = children[bestChild];
            BuildRecord lrecord(depth+1);
            BuildRecord rrecord(depth+1);
            builder->partition(brecord,lrecord,rrecord);
            
            /* find new splits */
            lrecord.split = builder->find(lrecord);
            rrecord.split = builder->find(rrecord);
            
            /* temporal split */
            if (brecord.split.data == -1) {
              active[bestChild] = false;
              active[numChildren] = true;
            } 
            /* standard split */
            else {
              active[bestChild] = true;
              active[numChildren] = true;
            }
            children[bestChild  ] = lrecord;
            children[numChildren] = rrecord; 
            numChildren++;
          }

          __forceinline size_t size() const {
            return numChildren;
          }

          __forceinline BuildRecord& operator[] ( size_t i ) {
            return children[i];
          }

          bool hasTimeSplits() const {
            return false;
          }

          __forceinline void restore(size_t childID) {
          }

          __forceinline ssize_t best()
          {
            /*! find best child to split */
            float bestSAH = neg_inf;
            ssize_t bestChild = -1;
            for (size_t i=0; i<numChildren; i++) 
            {
              //if (!active[i]) continue;
              if (children[i].pinfo.size() <= builder->minLeafSize) continue; 
              if (expectedApproxHalfArea(children[i].pinfo.geomBounds) > bestSAH) {
                bestChild = i; bestSAH = expectedApproxHalfArea(children[i].pinfo.geomBounds); 
              } 
            }
            return bestChild;
          }

        public:
          GeneralBVHMBBuilder* builder;
          BuildRecord children[MAX_BRANCHING_FACTOR];
          bool active[MAX_BRANCHING_FACTOR];
          size_t numChildren;
          size_t depth;
        };

        GeneralBVHMBBuilder (Scene* scene,
                             const ReductionTy& identity,
                             CreateAllocFunc& createAlloc, 
                             CreateNodeFunc& createNode, 
                             UpdateNodeFunc& updateNode, 
                             CreateLeafFunc& createLeaf,
                             ProgressMonitor& progressMonitor,
                             const PrimInfo& pinfo,
                             const size_t branchingFactor, const size_t maxDepth, 
                             const size_t logBlockSize, const size_t minLeafSize, const size_t maxLeafSize,
                             const float travCost, const float intCost)
          : HeuristicMBlur<Mesh,NUM_OBJECT_BINS>(scene), 
          identity(identity), 
          createAlloc(createAlloc), createNode(createNode), updateNode(updateNode), createLeaf(createLeaf), 
          progressMonitor(progressMonitor),
          pinfo(pinfo), 
          branchingFactor(branchingFactor), maxDepth(maxDepth),
          logBlockSize(logBlockSize), minLeafSize(minLeafSize), maxLeafSize(maxLeafSize),
          travCost(travCost), intCost(intCost)
        {
          if (branchingFactor > MAX_BRANCHING_FACTOR)
            throw_RTCError(RTC_UNKNOWN_ERROR,"bvh_builder: branching factor too large");
        }
        
        const ReductionTy createLargeLeaf(BuildRecord& current, Allocator alloc)
        {
          /* this should never occur but is a fatal error */
          if (current.depth > maxDepth) 
            throw_RTCError(RTC_UNKNOWN_ERROR,"depth limit reached");

          /* create leaf for few primitives */
          if (current.pinfo.size() <= maxLeafSize)
            return createLeaf(current,alloc);
          
          /* fill all children by always splitting the largest one */
          ReductionTy values[MAX_BRANCHING_FACTOR];
          BuildRecord children[MAX_BRANCHING_FACTOR];
          size_t numChildren = 1;
          children[0] = current;
        
          do {
            
            /* find best child with largest bounding box area */
            size_t bestChild = -1;
            size_t bestSize = 0;
            for (size_t i=0; i<numChildren; i++)
            {
              /* ignore leaves as they cannot get split */
              if (children[i].pinfo.size() <= maxLeafSize)
                continue;
              
              /* remember child with largest size */
              if (children[i].pinfo.size() > bestSize) { 
                bestSize = children[i].pinfo.size();
                bestChild = i;
              }
            }
            if (bestChild == (size_t)-1) break;
            
            /*! split best child into left and right child */
            BuildRecord left(current.depth+1);
            BuildRecord right(current.depth+1);
            Heuristic::splitFallback(children[bestChild].prims,left.pinfo,left.prims,right.pinfo,right.prims);
            left .split = find(left );
            right.split = find(right);
            
            /* add new children left and right */
            children[bestChild] = children[numChildren-1];
            children[numChildren-1] = left;
            children[numChildren+0] = right;
            numChildren++;
            
          } while (numChildren < branchingFactor);
          
          /* create node */
          BuildRecord* records[MAX_BRANCHING_FACTOR];
          for (size_t i=0; i<numChildren; i++) records[i] = &children[i];
          auto node = createNode(current,records,numChildren,alloc);
          
          /* recurse into each child  and perform reduction */
          for (size_t i=0; i<numChildren; i++)
            values[i] = createLargeLeaf(children[i],alloc);
          
          /* perform reduction */
          return updateNode(node,current.prims,values,numChildren);
        }
        
        __forceinline const typename Heuristic::Split find(BuildRecord& current) {
          return Heuristic::find (current.prims,current.pinfo,logBlockSize);
        }
        
        __forceinline void partition(BuildRecord& brecord, BuildRecord& lrecord, BuildRecord& rrecord) {
          Heuristic::split(brecord.split,brecord.pinfo,brecord.prims,lrecord.pinfo,lrecord.prims,rrecord.pinfo,rrecord.prims);
        }
        
        const ReductionTy recurse(BuildRecord& current, Allocator alloc, bool toplevel)
        {
          if (alloc == nullptr)
            alloc = createAlloc();

          /* call memory monitor function to signal progress */
          if (toplevel && current.size() <= SINGLE_THREADED_THRESHOLD)
            progressMonitor(current.size());
          
          /*! compute leaf and split cost */
          const float leafSAH  = intCost*current.pinfo.leafSAH(logBlockSize);
          const float splitSAH = travCost*current.pinfo.halfArea()+intCost*current.split.splitSAH();
          assert((current.pinfo.size() == 0) || ((leafSAH >= 0) && (splitSAH >= 0)));
          
          /*! create a leaf node when threshold reached or SAH tells us to stop */
          if (current.pinfo.size() <= minLeafSize || current.depth+MIN_LARGE_LEAF_LEVELS >= maxDepth || (current.pinfo.size() <= maxLeafSize && leafSAH <= splitSAH)) {
            Heuristic::deterministic_order(current.prims);
            return createLargeLeaf(current,alloc);
          }
          
          /*! initialize child list */
          ReductionTy values[MAX_BRANCHING_FACTOR];
#if MBLUR_NEW_ARRAY
          LocalChildList children(this,current);
#else
          LocalTree children(this,current);
#endif     
          //children.add(current);
          
          /*! split until node is full or SAH tells us to stop */
          do {
            ssize_t bestChild = children.best();
            if (bestChild == -1) break;
            children.split(bestChild);
          } while (children.size() < branchingFactor);
          
          /* sort buildrecords for simpler shadow ray traversal */
          //std::sort(&children[0],&children[children.size()],std::greater<BuildRecord>()); // FIXME: reduces traversal performance of bvh8.triangle4 (need to verified) !!
          
          /*! create an inner node */
          BuildRecord* records[MAX_BRANCHING_FACTOR];
          for (size_t i=0; i<children.size(); i++) records[i] = &children[i];
          auto node = createNode(current,records,children.size(),alloc);
          //auto node = createNode(current,children.children,children.size(),alloc);
          //for (size_t i=0; i<children.size(); i++) children[i].parent = records[i].parent;
          
          /* spawn tasks */
          if (current.size() > SINGLE_THREADED_THRESHOLD && !children.hasTimeSplits()) 
          {
            /*! parallel_for is faster than spawing sub-tasks */
            parallel_for(size_t(0), children.size(), [&] (const range<size_t>& r) {
                for (size_t i=r.begin(); i<r.end(); i++) {
                  values[i] = recurse(children[i],nullptr,true); 
                  _mm_mfence(); // to allow non-temporal stores during build
                }                
              });
            /* perform reduction */
            return updateNode(node,current.prims,values,children.size());
          }
          /* recurse into each child */
          else 
          {
            //for (size_t i=0; i<numChildren; i++)
            for (ssize_t i=children.size()-1; i>=0; i--) {
              children.restore(i);
              values[i] = recurse(children[i],alloc,false);
            }
            
            /* perform reduction */
            return updateNode(node,current.prims,values,children.size());
          }
        }
        
        /*! builder entry function */
        __forceinline const ReductionTy operator() (BuildRecord& record)
        {
          //BuildRecord br(record);
          record.split = find(record); 
          ReductionTy ret = recurse(record,nullptr,true);
          _mm_mfence(); // to allow non-temporal stores during build
          return ret;
        }
        
      private:
        const ReductionTy identity;
        CreateAllocFunc& createAlloc;
        CreateNodeFunc& createNode;
        UpdateNodeFunc& updateNode;
        CreateLeafFunc& createLeaf;
        ProgressMonitor& progressMonitor;
        
      private:
        const PrimInfo& pinfo;
        const size_t branchingFactor;
        const size_t maxDepth;
        const size_t logBlockSize;
        const size_t minLeafSize;
        const size_t maxLeafSize;
        const float travCost;
        const float intCost;
      };
  }
}
