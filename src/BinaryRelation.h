#pragma once

#include "UnionFind.h"
#include "Util.h"
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <tbb/concurrent_hash_map.h>

namespace souffle {
template <typename TupleType>
class BinaryRelation {
    typedef typename TupleType::value_type DomainInt;
    enum { arity = TupleType::arity };

    // Although breaking styleguide - this is marked mutable in order to ensure const-ness fits the other data
    // types
    // It is necessary as "read-only" operations such as iterator generation collapses the disjoint set tree
    // implicitly (causing writes)
    mutable souffle::SparseDisjointSet<DomainInt> sds;

    // read/write lock on orderedStates
    mutable souffle::shared_mutex statesLock;

    mutable std::atomic<bool> statesMapStale;

    // the ordering of states per disjoint set (mapping from representative to trie)
    // just a cache
    typedef souffle::BlockList<DomainInt> StatesList;
    typedef std::shared_ptr<StatesList> StatesBucket;
    typedef tbb::concurrent_hash_map<DomainInt, StatesBucket> StatesMap;
    mutable StatesMap orderedStates;

public:

    BinaryRelation() : statesMapStale(false) {};

    // TODO: what is this for, and is it correctly implemented?
    BinaryRelation& operator=(const BinaryRelation& old) {
        if (this == &old) return *this;

        sds = old.sds;
        orderedStates.clear();

        return *this;
    }

    /**
     * TODO: implement this operation_hint class
     * A collection of operation hints speeding up some of the involved operations
     * by exploiting temporal locality.
     */
    struct operation_hints {
        // resets all hints (to be triggered e.g. when deleting nodes)
        void clear() {}
    };

    /**
     * Insert the two values symbolically as a binary relation
     * @param x node to be added/paired
     * @param y node to be added/paired
     * @return true if the pair is new to the data structure
     */
    bool insert(DomainInt x, DomainInt y) {
        operation_hints z;
        return insert(x, y, z);
    };

    /**
     * Insert the two values symbolically as a binary relation
     * @param x node to be added/paired
     * @param y node to be added/paired
     * @param z the hints to where the pair should be inserted (not applicable atm)
     * @return true if the pair is new to the data structure
     */
    bool insert(DomainInt x, DomainInt y, operation_hints) {
        this->statesMapStale.store(true, std::memory_order_relaxed);
        //orderedStates.clear();
        bool retval = contains(x, y);
        sds.unionNodes(x, y);
        return retval;
    }

    /**
     * inserts all nodes from the other relation into this one
     * @param other the binary relation from which to add nodes from
     */
    void insertAll(const BinaryRelation<TupleType>& other) {
        other.genAllDisjointSetLists();
        for (auto& keypair : other.orderedStates) {
            DomainInt rep = keypair.first;
            StatesBucket& sb = keypair.second;
            const size_t ksize = sb->size();

#pragma omp parallel for
            for (size_t i = 0; i < ksize; ++i) {
                // this messes up iterators
                // this->insert(rep, sb->get(i));
                this->sds.unionNodes(rep, sb->get(i));
            }
        }
        // invalidate iterators unconditionally
        this->statesMapStale.store(true, std::memory_order_relaxed);
    }

    /**
     * Conditionally inserts from another BinaryRelation to this one
     *  if any disjoint sets intersect across the two (i.e. if A is in both
     *  this and the other one, insert everything that is alongside A in
     *  the second BR into this one.
     *  @param the other binaryrelation to read and insert from
     */
    void extend(const BinaryRelation<TupleType>& other) {
        this->genAllDisjointSetLists();
        other.genAllDisjointSetLists();
        // iterate over all elements for each dj set in this binrel
        for (auto& keypair : orderedStates) {
            // DomainInt rep = keypair.first;
            StatesBucket& sb = keypair.second;
            const size_t ksize = sb->size();

            // check if any elements in this djset exist in the other's domain
            for (size_t i = 0; i < ksize; ++i) {
                DomainInt c = sb->get(i);
                if (other.containsElement(c)) {
                    typename StatesMap::const_accessor a;
                    other.orderedStates.find(a, other.sds.readOnlyFindNode(c));
                    // union the two disjoint sets into this one
                    const StatesBucket& osb = a->second;
#pragma omp parallel for
                    for (size_t otherI = 0; otherI < osb->size(); ++otherI) {
                        // note, this does mess up iterators to be generated later - but we don't mind
                        this->sds.unionNodes(c, osb->get(otherI));
                    }
                    break;
                }
            }
        }
        // invalidate iterators unconditionally
        this->statesMapStale.store(true, std::memory_order_relaxed);
    }

protected:
    bool containsElement(DomainInt e) const {
        return this->sds.nodeExists(e);
    }

public:
    /**
     * Returns whether there exists a pair with these two nodes
     * @param x front of pair
     * @param y back of pair
     */
    bool contains(DomainInt x, DomainInt y) const {
        return sds.contains(x, y);
    }

    void clear() {
        statesLock.lock();

        // we should be able to clear this prior, as it requires a lock on its own
        sds.clear();
        orderedStates.clear();

        statesLock.unlock();
    }

    /**
     * Size of relation
     * @return the sum of the number of pairs per disjoint set
     */
    size_t size() const {
        genAllDisjointSetLists();

        statesLock.lock_shared();

        size_t retVal = 0;
        for (auto& x : orderedStates) {
            const size_t s = x.second->size();
            retVal += s * s;
        }

        statesLock.unlock_shared();

        return retVal;
    }

private:
    // TODO: documentation (i.e. that this lazily makes the rep lists)
    // also, warning: if this is called during insertion, this will break... probably
    // all this function does, is insert each disjoint set into a separate key in a hashmap. We want this to
    // be as lazy as possible too.
    void genAllDisjointSetLists() const {
        statesLock.lock();

        // no need to generate again, already done.
        if (!this->statesMapStale.load(std::memory_order_acquire)) {
            statesLock.unlock();
            return;
        }

        // even though it might be partially stale (i.e. genDJSetList may have been
        // activated on one or more), we need to fully regen.
        orderedStates.clear();

        size_t dSetSize = sds.ds.a_blocks.size();
// go through the underlying djset, and try to insert into the hash map with key being
// the sparse representative
#pragma omp parallel for
        for (size_t i = 0; i < dSetSize; ++i) {
            typename TupleType::value_type sVal = sds.toSparse(i);

            parent_t rep = sds.readOnlyFindNode(sVal);

// TODO: do some magic with intel's accessor to try and keep it const_, and instead
// promote when necessary.
// currently, #define doublechecked is way faster with all within one dj set (makes sense) & is roughly the
// same with n disjoint sets (surprising, but nice)
#define doublechecked
#ifdef doublechecked
            // whether the piggy list is already created or not
            bool mayNeedCreation = !orderedStates.count(rep);
            // to filter false positives
            if (mayNeedCreation) {
                typename StatesMap::accessor a;
                bool needsCreation = orderedStates.insert(a, rep);
                // need to make the piggylist here much smaller (consider the case there's one per element
                // within the BR - memory bomb!)
                if (needsCreation) a->second = std::make_shared<StatesList>(1);

                // then simply insert the sparse value
                size_t pos = a->second->createNode();
                a->second->insertAt(pos, sVal);
            } else {
                typename StatesMap::const_accessor a;
                orderedStates.find(a, rep);

                // then simply insert the sparse value
                size_t pos = a->second->createNode();
                a->second->insertAt(pos, sVal);
            }
#else
            typename StatesMap::accessor a;
            bool isNew = orderedStates.insert(a, rep);
            if (isNew) a->second = std::make_shared<StatesList>(1);
            size_t pos = a->second->createNode();
            a->second->insertAt(pos, sVal);
#endif
        }

        // needed to allow iteration & find concurrently
        // https://www.threadingbuildingblocks.org/docs/help/reference/containers_overview/concurrent_hash_map_cls/concurrent_operations_hash.html
        orderedStates.rehash(orderedStates.size() * 2);

        statesMapStale.store(false, std::memory_order_release);

        statesLock.unlock();
    }

    // warning: if this is called during insertion, this will break... probably
    std::shared_ptr<souffle::BlockList<DomainInt>> genDJSetList(DomainInt val) const {
        if (!sds.nodeExists(val)) {
            std::cerr << "cannot generate for a non-existent value";
            throw "cannot generate for a non-existent value";
        }

        statesLock.lock();

        // ensure that we have the highest rep
        DomainInt rep = sds.readOnlyFindNode(val);

        // we don't need to generate it (as all are generated)
        if (!this->statesMapStale.load(std::memory_order_acquire)) {
            typename StatesMap::const_accessor a;
            assert(orderedStates.find(a, rep) && "somehow doesn't exist despite being non-stale.");
            statesLock.unlock();
            return a->second;
        }

        // otherwise we need to check if this one /has/ been generated by this fn already
        typename StatesMap::accessor a;
        bool isNew = orderedStates.insert(a, rep);

        // map is already generated
        if (!isNew) {
            statesLock.unlock();
            return a->second;
        }

        // just get a ref
        a->second = std::make_shared<StatesList>();
        StatesBucket& sp = a->second;

        const parent_t dVal = sds.toDense(val);
        const size_t dSetSize = sds.ds.a_blocks.size();
        const auto& de = this->sds.ds;
        // go through direct access of the block list, and append those that are in

#pragma omp parallel for
        for (size_t i = 0; i < dSetSize; ++i) {
            // XXX: not necessary atm, as we findnode on i
            // parent_t c = DisjointSet::b2p(dsblocks.get(i));

            // c is a member of dVal's djset, so we insert it in this list
            if (de.readOnlyFindNode(i) == dVal) {
                size_t pos = sp->createNode();
                sp->insertAt(pos, sds.toSparse(i));
            }
        }

        orderedStates.rehash(orderedStates.size() * 2);
        statesLock.unlock();

        return a->second;
    }

public:
    // an almighty iterator for several types of iteration.
    // Unfortunately, subclassing isn't an option with souffle
    //   - we don't deal with pointers (so no virtual)
    //   - and a single iter type is expected (see Relation::iterator e.g.) (i think)
    class iterator : public std::iterator<std::forward_iterator_tag, TupleType> {
        const BinaryRelation* br = nullptr;
        // special tombstone value to notify that this iter represents the end
        bool isEndVal = false;

        // all the different types of iterator this can be
        enum IterType { ALL, ANTERIOR, ANTPOST, WITHIN };
        IterType ityp;

        TupleType cPair;

        // the disjoint set that we're currently iterating through
        StatesBucket djSetList;

        // all disjoint set lists that exist..?
        typename StatesMap::iterator djSetMapListIt;
        // just a cached value
        typename StatesMap::iterator djSetMapListEnd;

        // used for ALL, and POSTERIOR (just a current index in the cList)
        size_t cAnteriorIndex = 0;
        // used for ALL, and ANTERIOR (just a current index in the cList)
        size_t cPosteriorIndex = 0;

    public:
        // one iterator for signalling the end (simplifies)
        explicit iterator(const BinaryRelation* br, bool /* signalIsEndIterator */)
                : br(br), isEndVal(true){};

        // ALL: iterator for iterating over everything (i.e. (_, _))
        explicit iterator(const BinaryRelation* br)
                : br(br), ityp(IterType::ALL), 
                djSetMapListIt(br->orderedStates.begin()), djSetMapListEnd(br->orderedStates.end())
                   {
            // we called begin on an empty dj set
            if (djSetMapListIt == djSetMapListEnd) {
                isEndVal = true;
                return;
            }
            djSetList = (*djSetMapListIt).second;
            assert(djSetList->size() != 0);

            updateAnterior();
            updatePosterior();
        };

        // WITHIN: iterator for everything within the same DJset (used for BinaryRelation.partition())
        explicit iterator(const BinaryRelation* br, const StatesBucket within)
                : br(br), ityp(IterType::WITHIN), djSetList(within) {
            // empty dj set
            if (djSetList->size() == 0) {
                isEndVal = true;
            }

            updateAnterior();
            updatePosterior();
        }

        // ANTERIOR: iterator that yields all (former, _) \in djset(former) (djset(former) === within)
        explicit iterator(const BinaryRelation* br, const DomainInt former, const StatesBucket within)
                : br(br), ityp(IterType::ANTERIOR), djSetList(within) {
            if (djSetList->size() == 0) {
                isEndVal = true;
            }

            setAnterior(former);
            updatePosterior();
        }

        // ANTPOST: iterator that yields all (former, latter) \in djset(former), (djset(former) ==
        // djset(latter) == within)
        explicit iterator(
                const BinaryRelation* br, const DomainInt former, DomainInt latter, const StatesBucket within)
                : br(br), ityp(IterType::ANTPOST), djSetList(within) {
            if (djSetList->size() == 0) {
                isEndVal = true;
            }

            setAnterior(former);
            setPosterior(latter);
        }

        /** explicit set first half of cPair */
        inline void setAnterior(const DomainInt a) {
            this->cPair[0] = a;
        }

        /** quick update to whatever the current index is pointing to */
        inline void updateAnterior() {
            this->cPair[0] = this->djSetList->get(this->cAnteriorIndex);
        }

        /** explicit set second half of cPair */
        inline void setPosterior(const DomainInt b) {
            this->cPair[1] = b;
        }

        /** quick update to whatever the current index is pointing to */
        inline void updatePosterior() {
            this->cPair[1] = this->djSetList->get(this->cPosteriorIndex);
        }

        // copy ctor
        iterator(const iterator& other) = default;
        // move ctor
        iterator(iterator&& other) = default;
        // assign iter
        iterator& operator=(const iterator& other) = default;

        bool operator==(const iterator& other) const {
            if (isEndVal && other.isEndVal) return br == other.br;
            return isEndVal == other.isEndVal && cPair == other.cPair;
        }

        bool operator!=(const iterator& other) const {
            return !((*this) == other);
        }

        const TupleType& operator*() const {
            return cPair;
        }

        const TupleType* operator->() const {
            return &cPair;
        }

        /* pre-increment */
        iterator& operator++() {
            if (isEndVal) throw "error: incrementing an out of range iterator";

            switch (ityp) {
                case IterType::ALL:
                    // move posterior along one
                    // see if we can't move the posterior along
                    if (++cPosteriorIndex == djSetList->size()) {
                        // move anterior along one
                        // see if we can't move the anterior along one
                        if (++cAnteriorIndex == djSetList->size()) {
                            // move the djset it along one
                            // see if we can't move it along one (we're at the end)
                            if (++djSetMapListIt == djSetMapListEnd) {
                                isEndVal = true;
                                return *this;
                            }

                            // we can't iterate along this djset if it is empty
                            djSetList = (*djSetMapListIt).second;
                            if (djSetList->size() == 0) throw "error: encountered a zero size djset";

                            // update our cAnterior and cPosterior
                            cAnteriorIndex = 0;
                            cPosteriorIndex = 0;
                            updateAnterior();
                            updatePosterior();
                        }

                        // we moved our anterior along one
                        updateAnterior();

                        cPosteriorIndex = 0;
                        updatePosterior();
                    }
                    // we just moved our posterior along one
                    updatePosterior();

                    break;
                case IterType::ANTERIOR:
                    // step posterior along one, and if we can't, then we're done.
                    if (++cPosteriorIndex == djSetList->size()) {
                        isEndVal = true;
                        return *this;
                    }
                    updatePosterior();

                    break;
                case IterType::ANTPOST:
                    // fixed anterior and posterior literally only points to one, so if we increment, its the
                    // end
                    isEndVal = true;
                    break;
                case IterType::WITHIN:
                    // move posterior along one
                    // see if we can't move the posterior along
                    if (++cPosteriorIndex == djSetList->size()) {
                        // move anterior along one
                        // see if we can't move the anterior along one
                        if (++cAnteriorIndex == djSetList->size()) {
                            isEndVal = true;
                            return *this;
                        }

                        // we moved our anterior along one
                        updateAnterior();

                        cPosteriorIndex = 0;
                        updatePosterior();
                    }
                    // we just moved our posterior along one
                    updatePosterior();
                    break;
            }

            return *this;
        }
    };

public:
    /**
     * iterator pointing to the beginning of the tuples, with no restrictions
     * @return the iterator that corresponds to the beginning of the binary relation
     */
    iterator begin() const {
        genAllDisjointSetLists();
        return iterator(this);
    }

    /**
     * iterator pointing to the end of the tuples
     * @return the iterator which represents the end of the binary rel
     */
    iterator end() const {
        return iterator(this, true);
    }

    /**
     * Obtains a range of elements matching the prefix of the given entry up to
     * levels elements.
     *
     * @tparam levels the length of the requested matching prefix
     * @param entry the entry to be looking for
     * @return the corresponding range of matching elements
     */
    template <unsigned levels>
    range<iterator> getBoundaries(const operation_hints& entry) const {
        operation_hints ctxt;
        return getBoundaries<levels>(entry, ctxt);
    }

    /**
     * Obtains a range of elements matching the prefix of the given entry up to
     * levels elements. A operation context may be provided to exploit temporal
     * locality.
     *
     * @tparam levels the length of the requested matching prefix
     * @param entry the entry to be looking for
     * @param ctxt the operation context to be utilized
     * @return the corresponding range of matching elements
     */
    template <unsigned levels>
    range<iterator> getBoundaries(const TupleType& entry, operation_hints&) const {
        // TODO: use ctxt to exploit locality - does this really matter

        // if nothing is bound => just use begin and end
        if (levels == 0) return make_range(begin(), end());

        // as disjoint set is exactly two args (equiv relation)
        // we only need to handle these cases

        if (levels == 1) {
            // need to test if the entry actually exists
            if (!sds.nodeExists(entry[0])) return make_range(end(), end());

            // return an iterator over all (entry[0], _)
            return make_range(anteriorIt(entry[0]), end());
        }

        if (levels == 2) {
            // need to test if the entry actually exists
            if (!sds.contains(entry[0], entry[1])) return make_range(end(), end());

            // if so return an iterator containing exactly that node
            return make_range(antpostit(entry[0], entry[1]), end());
        }

        std::cerr << "invalid state, cannot search for >2 arg start point in getBoundaries, in 2 arg tuple "
                     "store\n";
        throw "invalid state, cannot search for >2 arg start point in getBoundaries, in 2 arg tuple store";

        return make_range(end(), end());
    }

    /**
     * Creates an iterator that generates all pairs (A, X)
     * for a given A, and X are elements within A's disjoint set.
     * @param anteriorVal: The first value of the tuple to be generated for
     * @return the iterator representing this.
     */
    iterator anteriorIt(DomainInt anteriorVal) const {
        
        genAllDisjointSetLists();

        // locate the blocklist that the anterior val resides in
        StatesBucket found;
        {
            typename StatesMap::const_accessor a;
            bool f = orderedStates.find(a, anteriorVal);
            assert(f && "uhh.... how did this happen");
            found = a->second;
        }

        return iterator(this, anteriorVal, found);
    }

    /**
     * Creates an iterator that generates the pair (A, B)
     * for a given A and B. If A and B don't exist, or aren't in the same set,
     * then the end() iterator is returned.
     * @param anteriorVal: the A value of the tuple
     * @param posteriorVal: the B value of the tuple
     * @return the iterator representing this
     */
    iterator antpostit(DomainInt anteriorVal, DomainInt posteriorVal) const {
        // obv if they're in diff sets, then iteration for this pair just ends.
        if (!sds.sameSet(anteriorVal, posteriorVal)) return end();
        
        genAllDisjointSetLists();

        // locate the blocklist that the val resides in
        StatesBucket found;
        {
            typename StatesMap::const_accessor a;
            bool f = orderedStates.find(a, posteriorVal);
            assert(f && "uhh.... how did this happen");
            found = a->second;
        }


        return iterator(this, anteriorVal, posteriorVal, found);
    }

    /**
     * Begin an iterator over all pairs within a single disjoint set - This is used for partition().
     * @param rep the representative of (or element within) a disjoint set of which to generate all pairs
     * @return an iterator that will generate all pairs within the disjoint set
     */
    iterator closure(DomainInt rep) const {
        genAllDisjointSetLists();

        // locate the blocklist that the val resides in
        StatesBucket found;
        {
            typename StatesMap::const_accessor a;
            bool f = orderedStates.find(a, rep);
            assert(f && "uhh.... how did this happen");
            found = a->second;
        }

        return iterator(this, found);
    }

    /**
     * Generate an approximate number of iterators for parallel iteration
     * The iterators returned are not necessarily equal in size, but in practise are approximately similarly
     * sized
     * Depending on the structure of the data, there can be more or less partitions returned than requested.
     * @param chunks the number of requested partitions
     * @return a list of the iterators as ranges
     */
    std::vector<souffle::range<iterator>> partition(size_t chunks) const {
        // generate all reps
        genAllDisjointSetLists();

        size_t numPairs = this->size();
        if (numPairs == 0) return {};
        if (numPairs == 1 || chunks <= 1) return {souffle::make_range(begin(), end())};

        // if there's more dj sets than requested chunks, then just return an iter per dj set
        std::vector<souffle::range<iterator>> ret;
        if (chunks <= orderedStates.size()) {
            for (auto& p : orderedStates) {
                ret.push_back(souffle::make_range(closure(p.first), end()));
            }
            return ret;
        }

        // keep it simple stupid 
        // just go through and if the size of the binrel is > numpairs/chunks, then generate an anteriorIt for each
        const size_t perchunk = numPairs/chunks;
        for (const auto& itp : orderedStates) {
            const size_t s = itp.second->size();
            if (s*s > perchunk) {
                for (const auto& i : *itp.second) {
                    ret.push_back(souffle::make_range(anteriorIt(i), end()));
                }
            } else {
                ret.push_back(souffle::make_range(closure(itp.first), end()));
            }
        }

        //typedef std::pair<size_t, DomainInt> osizeBuck;
        //// go through in descending order, and make iterators for them
        //std::vector<osizeBuck> orderedSizes;
        //// TODO: technically this is parellelisable? Dunno if O(dlogd) vs O(dlogd/T) is worth it. I guess it
        //// improves the worst case runtime scenario?
        //std::for_each(orderedStates.begin(), orderedStates.end(),
        //        [&](const std::pair<DomainInt, StatesBucket>& a) { orderedSizes.push_back(std::make_pair(a.second->size(), a.first)); });
        //std::sort(
        //        orderedSizes.begin(), orderedSizes.end(), [](osizeBuck& a, osizeBuck& b) { return a.first > b.first; });

        //auto shouldSplit = [](size_t djSize, size_t pairsRemaining, size_t remainingChunks) {
        //    return (djSize * djSize) > (pairsRemaining / remainingChunks);
        //};

        //// go through the disjoint sets in descending size, and test if whether to split or not
        //for (auto& pair : orderedSizes) {
        //    // if the disjoint set is too big, then make anterior iterators for each of the elements within it
        //    if (shouldSplit(pair.first, numPairs, chunks)) {
        //        // find the key in the hashmap
        //        typename StatesMap::const_accessor a;
        //        auto isfound = orderedStates.find(a, pair.second);
        //        assert(isfound && "pair doesn't exist");
        //        // for each in that dj set, add it in
        //        for (auto x : *a->second) {
        //            ret.push_back(souffle::make_range(anteriorIt(x), end()));
        //            --chunks;
        //        }
        //        numPairs -= pair.first * pair.first;
        //    } else {
        //        // the whole disjoint set in a single iter
        //        ret.push_back(souffle::make_range(closure(pair.second), end()));
        //        numPairs -= pair.first * pair.first;
        //        --chunks;
        //    }
        //}

        return ret;
    }
};
}  // namespace souffle
