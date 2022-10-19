//
//  IMPCaches.cpp
//  dyld_shared_cache_builder
//
//  Created by Thomas Deniau on 18/12/2019.
//

#include "FileAbstraction.hpp"
#include "IMPCaches.hpp"
#include "IMPCachesBuilder.hpp"
#include "ImpCachesBuilder.h"
#include "JSONReader.h"

#include <assert.h>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <string>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <numeric>
#include <random>
#include <map>

namespace IMPCaches {

std::string ClassData::description() const
{
    std::stringstream ss;
    ss << name << " modulo:" << modulo();
    if (isMetaclass) ss << " (metaclass)";
    return ss.str();
}

std::string ClassData::PlacementAttempt::description() const
{
    std::stringstream stream;
    stream << "needed bits: " << neededBits << ", shift: " << shift;
    return stream.str();
}

typename ClassData::PlacementAttempt ClassData::attemptForShift(int shiftToTry, int neededBitsToTry) const
{
    int totalNumberOfBitsToSet = 0;
    int mask = (1 << neededBitsToTry) - 1;
    for (const Method& method : methods) {
        totalNumberOfBitsToSet += method.selector->numberOfBitsToSet(shiftToTry, mask);
    }

    return ClassData::PlacementAttempt(totalNumberOfBitsToSet, shiftToTry, neededBitsToTry);
}

std::vector<typename ClassData::PlacementAttempt> ClassData::attempts() const
{
    // We have 26 MB of selectors, and among them only ~ 7MB are deemed
    // "interesting" to include in our hash tables.

    // So we should be able to fit on 24 bits of address space (~ 16 MB of selectors),
    // but need to keep the low 7 bits available so that we
    // don't have to worry about selector length and so that
    // we don't have to worry about collisions. (i.e. multiple selectors
    // end up at the same address with this algorithm and then we play
    // in another step with
    // the low 7 bits to give them all an unique address).
    // Then if there are holes given this 128-byte alignment, we can
    // fill the holes with selectors excluded from the algorithm.

    // Let us grow the hash tables to one more bit if needed
    // as the full problem is too difficult.
    std::vector<int>              allowedNeededBits { neededBits, neededBits + 1 };
    std::vector<PlacementAttempt> attempts;
    for (auto i : allowedNeededBits) {
        // Go thrugh all the possible shifts, starting at 0, knowing that
        // shift+neededBits needs to fit on 17 bits.
        
        for (int shiftToTry = 0; shiftToTry <= 17 - i; shiftToTry++) {
            attempts.push_back(attemptForShift(shiftToTry, i));
        }
    }
    sort(attempts.begin(), attempts.end());
    return attempts;
}

void ClassData::resetSlots()
{
#if DEBUG_SLOTS
    slots.assign(slots.size(), nullptr);
#else
    slots.assign(slots.size(), false);
#endif
}

void ClassData::backtrack(typename ClassData::PlacementAttempt::Result& resultToBacktrackFrom)
{
    if (!resultToBacktrackFrom.success) {
        // We backtrack from a failure if we decided to skip a class that was too difficult to place.
        // In this case there's nothing to do.
        
        return;
    }
    
    // Restore the addresses and masks we had in place before we did the step that let to resultToBacktrackFrom
    typename PlacementAttempt::PreviousState previousState = resultToBacktrackFrom.previousState;
    for (const Method& method : methods) {
        Selector* selector = method.selector;
        typename PlacementAttempt::PreviousMethodAddress previousMethod = previousState.methods[selector];
        selector->inProgressBucketIndex = previousMethod.address;
        selector->fixedBitsMask = previousMethod.fixedBitsMask;
    }

    shift = previousState.shift;
    neededBits = previousState.neededBits;
}

// Compute the number of needed bits for the hash table now that all the methods have been added
void ClassData::didFinishAddingMethods()
{
    if (methods.size() == 0) {
        neededBits = 0;
    } else {
        neededBits = (int)ceil(log2(double(methods.size())));
    }
}

bool ClassData::hadToIncreaseSize() const
{
    if (methods.size() == 0) return false;
    return neededBits > (int)(ceil(log2((double)(methods.size()))));
}

/// Try to see if we can make the shift and mask in @param attempt work.
typename ClassData::PlacementAttempt::Result ClassData::applyAttempt(const PlacementAttempt& attempt, std::minstd_rand & randomNumberGenerator)
{
    std::vector<Selector*> sortedMethods;
    sortedMethods.reserve(methods.size());
    for (const Method& method : methods) {
        sortedMethods.push_back(method.selector);
    }

    // Solve from most constrained to least constrained
    std::sort(sortedMethods.begin(), sortedMethods.end(), [attempt](Selector* m1, Selector* m2) {
        return m1->numberOfBitsToSet(attempt.shift, attempt.mask()) < m2->numberOfBitsToSet(attempt.shift, attempt.mask());
    });

    if (slots.size() < (1 << attempt.neededBits)) {
#if DEBUG_SLOTS
        slots.resize(1 << attempt.neededBits, nullptr);
#else
        slots.resize(1 << attempt.neededBits, false);
#endif
    }

    resetSlots();

    __block std::vector<int> addresses;
    for (auto m : sortedMethods) {
        __block bool found = false;

        // Check if all the bits are already assigned
        int shiftedMask = attempt.mask() << attempt.shift;
        if ((m->fixedBitsMask & shiftedMask) == shiftedMask) {
            int index = (m->inProgressBucketIndex >> attempt.shift) & attempt.mask();
            if (slots[index]) {
                typename ClassData::PlacementAttempt::Result result;
                result.success = false;
                return result;
            }
#if DEBUG_SLOTS
            slots[index] = m;
#else
            slots[index] = true;
#endif
            found = true;
            addresses.push_back(index);
        } else {
            // Some bits are not assigned yet, so try to find an address that would be compatible
            // with the existing bits for this method.

            int attemptModulo = 1 << attempt.neededBits;

            // Copied from std::shuffle
            auto forEachRandomNumber = [](int maxNumber, std::minstd_rand &g,
                                          void (^callback)(int current, bool& stop)) {
                // possibleAddresses = 0..<attemptModulo
                int possibleAddresses[maxNumber];
                std::iota(&possibleAddresses[0], &possibleAddresses[maxNumber], 0);

                int* first = &possibleAddresses[0];
                int* last = &possibleAddresses[maxNumber];

                typedef int* RandomAccessIterator;
                typedef typename std::iterator_traits<RandomAccessIterator>::difference_type difference_type;
                typedef std::uniform_int_distribution<ptrdiff_t> Dp;
                typedef typename Dp::param_type Pp;
                difference_type d = last - first;
                if (d > 1)
                {
                    Dp uid;
                    for (--last, (void) --d; first < last; ++first, (void)--d)
                    {
                        difference_type i = uid(g, Pp(0, d));
                        if (i != difference_type(0))
                            std::swap(*first, *(first + i));

                        bool stop = false;
                        callback(*first, stop);
                        if ( stop )
                            return;
                    }
                } else {
                    bool stop = false;
                    callback(*first, stop);
                }
            };

            // We randomize the addresses to try so that two random selectors
            // have as many ranges of different bits as possible, in order
            // to find a satisfying shift for every class.
            forEachRandomNumber(attemptModulo, randomNumberGenerator, ^(int i, bool &stop) {
                int futureAddress = m->inProgressBucketIndex | (i << attempt.shift);
                int slot = (futureAddress >> attempt.shift) & attempt.mask();

                // Make sure the new address is compatible with the existing bits
                bool addressesMatch = (futureAddress & m->fixedBitsMask) == (m->inProgressBucketIndex & m->fixedBitsMask);
                if (addressesMatch && !slots[slot]) {
#if DEBUG_SLOTS
                    slots[slot] = m;
#else
                    slots[slot] = true;
#endif
                    found = true;
                    addresses.push_back(i);
                    stop = true;
                }
            });
            if (!found) {
                typename ClassData::PlacementAttempt::Result result;
                result.success = false;
                return result;
            }
        }
    }

    // We succeeded, record the state so that we can backtrack if needed
    std::unordered_map<Selector*, typename PlacementAttempt::PreviousMethodAddress> previousMethods;
    for (unsigned long i = 0; i < sortedMethods.size(); i++) {
        Selector* m = sortedMethods[i];
        int         previousAddress = m->inProgressBucketIndex;
        int         previousMask = m->fixedBitsMask;
        m->inProgressBucketIndex |= addresses[i] << attempt.shift;
        m->fixedBitsMask |= attempt.mask() << attempt.shift;
        previousMethods[m] = typename PlacementAttempt::PreviousMethodAddress {
            .address = previousAddress,
            .fixedBitsMask = previousMask
        };
    }

    typename PlacementAttempt::PreviousState previousState {
        .neededBits = neededBits,
        .shift = shift,
        .methods = previousMethods
    };
    shift = attempt.shift;
    neededBits = attempt.neededBits;

    typename  PlacementAttempt::Result result {
        .success = true,
        .previousState = previousState
    };

    return result;
}

bool ClassData::checkConsistency() {
    resetSlots();
    for (const Method& method : methods) {
        const Selector* s = method.selector;
        int slotIndex = (s->inProgressBucketIndex >> shift) & mask();
        if (slots[slotIndex]) {
            return false;
        }
#if DEBUG_SLOTS
        slots[slotIndex] = s;
#else
        slots[slotIndex] = true;
#endif
    }
    return true;
}

Constraint ClassData::constraintForMethod(const Selector* method) {
    resetSlots();
    allowedValues.clear();
    
    // Fill the slots with all our methods except `method`
    for (const Method& m : methods) {
        const Selector* s = m.selector;
        if (s == method) {
            continue;
        }

        int slotIndex = (s->inProgressBucketIndex >> shift) & mask();
#if DEBUG_SLOTS
        assert(slots[slotIndex] == nullptr);
        slots[slotIndex] = s;
#else
        assert(!slots[slotIndex]);
        slots[slotIndex] = true;
#endif
    }

    // What are the remaining empty slots in which we could put `method`?
    int max = 1 << neededBits;
    for (int i = 0 ; i < max ; i++) {
        if (!slots[i]) {
            allowedValues.push_back(i);
        }
    }

    auto allowedSet = std::unordered_set<int>(allowedValues.begin(), allowedValues.end());
    return Constraint {
        .mask = mask(),
        .shift = shift,
        .allowedValues = allowedSet
    };
}

int AddressSpace::sizeAtIndex(int idx) const
{
    if (sizes.find(idx) != sizes.end()) {
        return sizes.at(idx);
    } else {
        return 0;
    }
}

void AddressSpace::removeUninterestingSelectors() {
    for (auto& [k,v] : methodsByIndex) {
        v.erase(std::remove_if(v.begin(), v.end(), [](const Selector* s){
            return s->classes.size() == 0;
        }), v.end());
    }
}

int AddressSpace::sizeAvailableAfterIndex(int idx) const
{
    int availableAfterThisAddress = bagSizeAtIndex(idx) - sizeAtIndex(idx);
    for (int j = idx + 1; j < maximumIndex; j++) {
        if (methodsByIndex.find(j) != methodsByIndex.end()) {
            break;
        } else {
            availableAfterThisAddress += bagSizeAtIndex(j);
        }
    }

    return availableAfterThisAddress;
}

// Because some selectors are longer than 128 bytes, we have to sometimes let
// them overflow into the next 128-byte bucket. This method tells you if you
// can place a method in a bucket without colliding with an overflowing selector
// from one of the previous buckets.
bool AddressSpace::canPlaceWithoutFillingOverflowCellAtIndex(int idx) const
{
    if ((idx == 0) || (sizeAtIndex(idx) > 0)) {
        return true;
    }

    int j = idx;
    int availableOnOrBefore = 0;

    while (j > 0 && (sizeAtIndex(j) == 0)) {
        availableOnOrBefore += bagSizeAtIndex(j);
        j -= 1;
    }

    int sizeOfFirstNonEmptyCellBefore = sizeAtIndex(j);
    return (sizeOfFirstNonEmptyCellBefore < availableOnOrBefore);
}

bool AddressSpace::canPlaceMethodAtIndex(const Selector* method, int idx) const
{
    int  existingSize = sizeAtIndex(idx);
    bool canPlaceWithoutFillingOverflow = canPlaceWithoutFillingOverflowCellAtIndex(idx);

    if (!canPlaceWithoutFillingOverflow) {
        return false;
    }

    int  available = bagSizeAtIndex(idx) - existingSize;
    int  methodSize = method->size();
    bool enoughRoom = available > methodSize;

    if (enoughRoom) {
        return true;
    }

    bool tooBigButOverflowExists = (methodSize > 64) && (available > 0) && (sizeAvailableAfterIndex(idx) > methodSize);

    return tooBigButOverflowExists;
}

void AddressSpace::placeMethodAtIndex(Selector* method, int idx)
{
    auto [it, success] = methodsByIndex.try_emplace(idx, std::vector<Selector*>());
    it->second.push_back(method);

    auto [it2, success2] = sizes.try_emplace(idx, 0);
    it2->second += method->size();
}

// At this point selected are already sorted into 128-byte buckets.
// Now fill in the low 7 bits of each address, and return a list
// of intervals [  ..... selector data....][ ...... hole ....][.... selector data...]
// so that we can stuff in selectors that don't participate in
// static IMP caches
void AddressSpace::computeLowBits(HoleMap& selectors) const {
    int currentEndOffset =  magicSelector.length() + 1;

    std::set<IMPCaches::Hole> & holes = selectors.holes;
    holes.clear();

    std::vector<int> orderedIndices;
    for (const auto& [index, selectors2] : methodsByIndex) {
        orderedIndices.push_back(index);
    }
    std::sort(orderedIndices.begin(), orderedIndices.end());
    for (int index : orderedIndices) {
        const std::vector<Selector*> & selectorsAtThisIndex = methodsByIndex.at(index);
        int bucketOffset = index << bagSizeShift;
        if (bucketOffset  > currentEndOffset) {
            holes.insert(Hole {
                .startAddress = currentEndOffset,
                .endAddress = bucketOffset,
            });
            currentEndOffset = bucketOffset;
        }
        for (Selector* s : selectorsAtThisIndex) {
            s->offset = currentEndOffset;
            currentEndOffset += s->size();
        }
    }

    selectors.endAddress = currentEndOffset;
}

int HoleMap::addStringOfSize(unsigned size) {
//    static int i = 0;
//    if (i++ % 1000 == 0) {
//        printf("Inserted 1000 more strings, number of holes = %lu\n", holes.size());
//    }
    Hole neededHole = Hole {
        .startAddress = 0,
        .endAddress = static_cast<int>(size)
    };
    std::set<Hole>::iterator it = holes.lower_bound(neededHole);
    if (it == holes.end()) {
        // insert at the end
        int end = endAddress;
        endAddress += size;
        return end;
    } else {
        // Remove this hole and insert a smaller one instead

        int address = it->startAddress;
        Hole updatedHole = *it;
        updatedHole.startAddress += size;
        holes.erase(it);

        // Don't insert if the hole is empty or won't fit any selector
        if (updatedHole.size() > 1) {
            holes.insert(updatedHole);
        }
        return address;
    }
}

unsigned long HoleMap::totalHoleSize() const {
    unsigned long result = 0;
    for (const Hole& hole : holes) {
        result += hole.size();
    }
    return result;
}

std::ostream& operator<<(std::ostream& o, const HoleMap& m) {
    int size = 0;
    int count = 0;
    for (const Hole& h : m.holes) {
        if (h.size() == size) {
            count++;
        } else {
            o << count << " holes of size " << size << std::endl;
            size = h.size();
            count = 1;
        }
    }
    return o;
}

std::ostream& operator<<(std::ostream& o, const AddressSpace& a)
{
    int maximumIndex = 0;
    for (const auto& kvp : a.methodsByIndex) {
        maximumIndex = std::max(maximumIndex, kvp.first);
    }

    std::vector<double>                     lengths;
    std::map<int, std::vector<Selector*>> sortedMethodsByAddress;
    sortedMethodsByAddress.insert(a.methodsByIndex.begin(), a.methodsByIndex.end());

    std::map<int, int> sortedSizes;
    sortedSizes.insert(a.sizes.begin(), a.sizes.end());

    for (const auto& kvp : sortedSizes) {
        lengths.push_back(kvp.second);
    }

    for (const auto& kvp : sortedMethodsByAddress) {
        o << std::setw(5) << kvp.first << ": ";
        for (Selector* m : kvp.second) {
            o << m->name << " ";
        }
        o << "\n";
    }

    o << "Max address " << maximumIndex << "\n";
    o << "Average length " << (double)std::accumulate(lengths.begin(), lengths.end(), 0) / lengths.size() << "\n";

    return o;
}

// Returns a constraint that is the intersection of "this" and "other", i.e. a constraint for which the allowed values
// are the intersection of the allowed values of "this" and "other" (taking into account shift and mask)
Constraint Constraint::intersecting(const Constraint& other) const
{
    if ((mask == other.mask) && (shift == other.shift)) {
        // fast path
        std::unordered_set<int> intersection;
        for (int allowedValue : allowedValues) {
            if (other.allowedValues.find(allowedValue) != other.allowedValues.end()) {
                intersection.insert(allowedValue);
            }
        }

        return Constraint {
            .mask = mask,
            .shift = shift,
            .allowedValues = intersection
        };
    }

    int                  shiftedMask = mask << shift;
    int                  otherShift = other.shift;
    int                  otherMask = other.mask;
    int                  otherShiftedMask = other.mask << otherShift;
    int                  intersectionMask = shiftedMask & otherShiftedMask;
    const std::unordered_set<int>& otherAllowedValues = other.allowedValues;

    // Always make sure we start with the left-most mask as self
    if (shiftedMask < otherShiftedMask) {
        return other.intersecting(*this);
    }

    // If there are no real constraints on our side, just return the other
    if ((mask == 0) && (allowedValues.size() == 1) && (*(allowedValues.begin()) == 0)) {
        return other;
    }

    // If there are no real constraints on the other side, just return our constraint
    if ((otherMask == 0) && (otherAllowedValues.size() == 1) && (*(otherAllowedValues.begin()) == 0)) {
        return *this;
    }

    if (otherShift >= shift) {
        // [self..[other]..self]
        // Restrict the allowed values to make sure they have the right bits.
        int           shiftDifference = otherShift - shift;
        std::unordered_set<int> combinedAllowedValues;
        for (int v : allowedValues) {
            int val = (v >> shiftDifference) & otherMask;
            if (otherAllowedValues.find(val) != otherAllowedValues.end()) {
                combinedAllowedValues.insert(v);
            }
        }

        return Constraint {
            .mask = mask,
            .shift = shift,
            .allowedValues = combinedAllowedValues
        };
    }

    int highestBit = (int)fls(shiftedMask) - 1;
    int otherHighestBit = (int)fls(otherShiftedMask) - 1;
    int otherMaskLength = fls(otherMask + 1) - 1;

    if (otherShiftedMask < (1 << shift)) {
        // [self]....[other]
        // Start by shifting all the allowed values in self
        int           numberOfUnconstrainedBits = shift - otherHighestBit - 1;
        int           maxUnconstrained = 1 << numberOfUnconstrainedBits;
        std::set<int> includingUnrestrictedBits;

        if (numberOfUnconstrainedBits > 0) {
            for (const int allowed : allowedValues) {
                int shifted = allowed << numberOfUnconstrainedBits;
                for (int unconstrained = 0; unconstrained < maxUnconstrained; unconstrained++) {
                    // Mix in unrestricted bits, then shift by [other]'s length
                    includingUnrestrictedBits.insert((shifted | unconstrained) << otherMaskLength);
                }
            };
        } else {
            for (const int allowed : allowedValues) {
                // Shift all the values by [other]'s length
                includingUnrestrictedBits.insert(allowed << otherMaskLength);
            }
        }

        // Or in the values for [other]
        std::unordered_set<int> finalAllowedValues;
        for (const int allowed : includingUnrestrictedBits) {
            for (const int otherValue : otherAllowedValues) {
                finalAllowedValues.insert(allowed | otherValue);
            }
        }

        return Constraint {
            .mask = ((1 << (highestBit + 1)) - 1) >> otherShift,
            .shift = otherShift,
            .allowedValues = finalAllowedValues
        };

    } else {
        // Overlap.
        // [self....[other....self].....other].......
        // We need to
        // * determine the set of bits allowed in the intersection
        // * filter each set of values to keep only these
        // * do the cross-product

        // Bits in the intersection

        int           shiftDifference = shift - otherShift;
        std::set<int> selfIntersectingBits;
        for (const int v : allowedValues) {
            selfIntersectingBits.insert(((v << shift) & intersectionMask) >> shift);
        }
        std::set<int> otherIntersectingBits;
        for (const int v : otherAllowedValues) {
            otherIntersectingBits.insert(((v << otherShift) & intersectionMask) >> shift);
        }

        std::set<int> intersectingBits;
        std::set_intersection(selfIntersectingBits.begin(), selfIntersectingBits.end(), otherIntersectingBits.begin(), otherIntersectingBits.end(), std::inserter(intersectingBits, intersectingBits.begin()));

        std::unordered_set<int> values;
        // Swift here was constructing a list of values for self and other
        // filtered on which elements had the right values for intersectionMask
        // This would avoid the n^3 loop at the expense of some storage... FIXME
        for (const int intersecting : intersectingBits) {
            int intersectingShifted = intersecting << shift;
            for (int selfAllowed : allowedValues) {
                if (((selfAllowed << shift) & intersectionMask) == intersectingShifted) {
                    for (int otherAllowed : otherAllowedValues) {
                        if (((otherAllowed << otherShift) & intersectionMask) == intersectingShifted) {
                            values.insert((selfAllowed << shiftDifference) | otherAllowed);
                        }
                    }
                }
            }
        }

        return Constraint {
            .mask = (shiftedMask | otherShiftedMask) >> otherShift,
            .shift = otherShift,
            .allowedValues = values
        };
    }
}

static std::ostream& operator << (std::ostream& o, const std::unordered_set<int> & s) {
    o << "{";
    for (int i : s) {
        o << i << ", ";
    }
    o << "}";
    return o;
}

std::ostream& operator << (std::ostream& o, const Constraint& c) {
    o << "(x >> " << c.shift << " & " << c.mask << " == " << c.allowedValues;
    return o;
}

bool ConstraintSet::add(const Constraint& c) {
    if (constraints.find(c) != constraints.end()) {
        return false;
    }
    constraints.insert(c);
    if (mergedConstraint.has_value()) {
        mergedConstraint = mergedConstraint->intersecting(c);
    } else {
        mergedConstraint = c;
    }
    return true;
}

void ConstraintSet::clear() {
    constraints.clear();
    mergedConstraint.reset();
}

bool IMPCachesBuilder::isClassInteresting(const ObjCClass& theClass) const {
    std::string_view classNameStr(theClass.className);
    if (theClass.isMetaClass) {
        return neededMetaclasses.find(classNameStr) != neededMetaclasses.end();
    } else {
        return neededClasses.find(classNameStr) != neededClasses.end();
    }
}

bool IMPCachesBuilder::isClassInterestingOrTracked(const ObjCClass& theClass) const {
    std::string_view classNameStr(theClass.className);
    auto& neededArray = theClass.isMetaClass ? neededMetaclasses : neededClasses;
    auto& trackedArray = theClass.isMetaClass ? trackedMetaclasses : trackedClasses;

    return (neededArray.find(classNameStr) != neededArray.end()) ||
           (trackedArray.find(classNameStr) != trackedArray.end());
}

void IMPCachesBuilder::addMethod(IMPCaches::ClassData* classDataPtr, std::string_view methodName, std::string_view installName, std::string_view className, std::string_view catName, bool inlined, bool fromFlattening) {
    std::string_view methodNameView(methodName);

    auto [selectorIterator, success] = selectors.map.try_emplace(methodNameView, std::make_unique<Selector>());
    IMPCaches::Selector* thisSelectorData = selectorIterator->second.get();
    if (success) {
        thisSelectorData->name = methodName;
    }

    std::vector<ClassData::Method> & methods = classDataPtr->methods;
    // Check in the existing methods to see if the method already exists...
    bool exists = false;
    for (const ClassData::Method& m : methods) {
        if (m.selector == thisSelectorData) {
            // printf("Preventing insertion of duplicate %s.%s\n", classDataPtr->name, methodName);
            exists = true;
            break;
        }
    }

    if (!exists) {
        ClassData::Method m {
            .installName = installName,
            .className = className,
            .categoryName = catName,
            .selector = thisSelectorData,
            .wasInlined = inlined,
            .fromFlattening = fromFlattening
        };

        thisSelectorData->classes.push_back(classDataPtr);
        classDataPtr->methods.push_back(m);
    }
}

void IMPCachesBuilder::inlineMethodIfNeeded(IMPCaches::ClassData* classToInlineIn, std::string_view classToInlineFrom, std::string_view catToInlineFrom, std::string_view installNameToInlineFrom, std::string_view name, std::set<Selector*>& seenSelectors, bool isFlattening) {
    std::string_view nameView(name);

    if ((nameView == ".cxx_construct") || (nameView == ".cxx_destruct")) {
        // These selectors should never be inherited.
        // object_cxxConstructFromClass / object_cxxDestructFromClass walk the class hierarchy and call them all.

        return;
    }

    bool shouldInline = isFlattening || (selectorsToInline.find(nameView) != selectorsToInline.end());
    if (shouldInline) {
        // The selector hasn't necessarily been seen at this point: eg. we don't build an IMP cache for UIView, so we haven't seen -[UIView superview] yet.

        auto [it, inserted] = selectors.map.try_emplace(nameView, std::make_unique<Selector>());
        IMPCaches::Selector* thisSelectorData = it->second.get();

        if (inserted) {
            thisSelectorData->name = name;
        }

        if (inserted || seenSelectors.find(thisSelectorData) == seenSelectors.end()) {
            seenSelectors.insert(thisSelectorData);
            const bool inlined = true;
            addMethod(classToInlineIn, name, installNameToInlineFrom, classToInlineFrom, catToInlineFrom, inlined, isFlattening);
        }
    }
}

// This goes through all the superclasses of the interesting classes so that
// we can track their methods for inlining.
// Since it goes through the superclasses, we also take this opportunity
// to add subclassses of duplicate classes to the duplicate classes set.
void IMPCachesBuilder::buildTrackedClasses(DylibState& dylib)
{
    for ( const imp_caches::Class& objcClass : dylib.inputDylib->classes ) {
        // The class might not be in the map as we exclude classes with missing weak superclasses
        auto classIt = objcClasses.find(&objcClass);
        if ( classIt == objcClasses.end() )
            continue;

        const auto& theClass = classIt->second;
        ClassKey theClassKey {
            .name = theClass.className,
            .metaclass = theClass.isMetaClass
        };

        if (!isClassInteresting(theClass))
            continue;

        // Go through superclasses and add them to the tracked set.
        const IMPCaches::IMPCachesBuilder::ObjCClass* currentClass = &theClass;
        bool rootClass = false;
        do {
            ClassKey k {
                .name = currentClass->className,
                .metaclass = currentClass->isMetaClass
            };

            // If one of the superclasses of theClass is in the duplicate classes set,
            // add theClass to the duplicate classes as well.
            if (duplicateClasses.find(k) != duplicateClasses.end()) {
                duplicateClasses.insert(theClassKey);
            }

            if (currentClass->isMetaClass) {
                trackedMetaclasses.insert(currentClass->className);
            } else {
                trackedClasses.insert(currentClass->className);
            }
            rootClass = currentClass->isRootClass;
            if (!rootClass) {
                // The superclass might not be in the map as we exclude classes with missing weak superclasses
                auto superclassIt = objcClasses.find(currentClass->superclass);
                if ( superclassIt == objcClasses.end() )
                    break;
                currentClass = &superclassIt->second;
            }
        } while (!rootClass);
    }
}

/// Parses the method lists of all the classes in @param dylib so that we populate the methods we want in each IMP cache skeleton.
void IMPCachesBuilder::populateMethodLists(DylibState& dylib, int* duplicateClassCount)
{
    for ( const imp_caches::Class& objcClass : dylib.inputDylib->classes ) {
        // The class might not be in the map as we exclude classes with missing weak superclasses
        auto classIt = objcClasses.find(&objcClass);
        if ( classIt == objcClasses.end() )
            continue;
        const auto& theClass = classIt->second;

        if (!isClassInterestingOrTracked(theClass))
            continue;

        bool interesting = isClassInteresting(theClass);

        std::string_view classNameString(theClass.className);
        std::unique_ptr<IMPCaches::ClassData> thisData = std::make_unique<ClassData>();
        IMPCaches::ClassData* thisDataPtr = thisData.get();
        thisData->name = theClass.className;
        thisData->isMetaclass = theClass.isMetaClass;
        thisData->shouldGenerateImpCache = interesting;

        for ( const imp_caches::Method& objcMethod : objcClass.methods ) {
            const bool inlined = false;
            const bool fromFlattening = false;
            addMethod(thisDataPtr, objcMethod.name, dylib.inputDylib->installName, theClass.className, "", inlined, fromFlattening);
        }

        ClassKey key {
            .name = classNameString,
            .metaclass = theClass.isMetaClass
        };
        assert(dylib.impCachesClassData.find(key) == dylib.impCachesClassData.end());

        if (duplicateClasses.find(key) != duplicateClasses.end()) {
            // We can't just set shouldGenerateImpCache to false ; we do it later
            // when we have built the flattening hierarchies in order to drop
            // any related classes as well.

            thisData->isPartOfDuplicateSet = true;
            *duplicateClassCount += 1;
        }

        dylib.impCachesClassData[key] = std::move(thisData);
    }
}

/// Parses all the categories within the same image as a class so that we can add the corresponding methods to the IMP cache skeletons, too.
void IMPCachesBuilder::attachCategories(DylibState& dylib)
{
    for ( const imp_caches::Category& objcCategory : dylib.inputDylib->categories ) {
        ObjCCategory previouslyFoundCategory = objcCategories.at(&objcCategory);

        if (previouslyFoundCategory.classDylib != dylib.inputDylib) {
            // Cross-image category
            continue;
        }

        // The class might not be in the map as we exclude classes with missing weak superclasses
        auto classIt = objcClasses.find(previouslyFoundCategory.cls);
        if ( classIt == objcClasses.end() )
            continue;
        const auto& theClass = classIt->second;

        auto& theMetaClass = objcClasses[theClass.metaClass];
        auto classNameString = std::string_view(theClass.className);

        if (isClassInterestingOrTracked(theClass)) {
            ClassKey key {
                .name = classNameString,
                .metaclass = false
            };
            ClassData* clsData = dylib.impCachesClassData.at(key).get();

            for ( const imp_caches::Method& objcMethod : objcCategory.instanceMethods ) {
                // The config file should specify only classes without cross-image categories, so we should have found a class here
                assert(clsData != NULL);
                std::string_view methodName = objcMethod.name;
                const bool inlined = false;
                const bool fromFlattening = false;
                addMethod(clsData, methodName, dylib.inputDylib->installName, theClass.className,
                          objcCategory.name, inlined, fromFlattening);
            }
        }
        if (isClassInterestingOrTracked(theMetaClass)) {
            ClassKey key {
                .name = classNameString,
                .metaclass = true
            };
            ClassData* metaclsData = dylib.impCachesClassData.at(key).get();

            for ( const imp_caches::Method& objcMethod : objcCategory.classMethods ) {
                assert(metaclsData != NULL);
                std::string_view methodName = objcMethod.name;
                const bool inlined = false;
                const bool fromFlattening = false;
                addMethod(metaclsData, methodName, dylib.inputDylib->installName, theMetaClass.className,
                          objcCategory.name, inlined, fromFlattening);
            }
        }
    }
}

struct FlatteningRootLookupResult {
    bool                        isInFlatteningHierarchy;
    const imp_caches::Class*    flatteningRootSuperclassLocation;
    ClassData::ClassLocator     flatteningRootSuperclass;
    std::set<std::string_view>  superclassesInFlatteningHierarchy;
    std::string_view            flatteningRootName;
};

static FlatteningRootLookupResult findFlatteningRoot(const imp_caches::Class& classLocation,
                                                     const std::unordered_map<const imp_caches::Class*, IMPCachesBuilder::ObjCClass> & objcClasses,
                                                     const std::unordered_set<std::string_view> & classHierarchiesToFlatten,
                                                     const std::unordered_set<std::string_view>  & metaclassHierarchiesToFlatten,
                                                     bool storeSuperclasses) {
    FlatteningRootLookupResult result;
    const imp_caches::Class* superclassLocation = &classLocation;
    __block bool rootClass = false;
    bool success = false;

    while (!rootClass) {
        const auto it = objcClasses.find(superclassLocation);
        if (it == objcClasses.end()) {
            break;
        }
        const IMPCachesBuilder::ObjCClass& iteratedClass = it->second;
        rootClass = iteratedClass.isRootClass;
        superclassLocation = iteratedClass.superclass;

        if (storeSuperclasses) {
            result.superclassesInFlatteningHierarchy.insert(iteratedClass.className);
        }

        bool metaClassBeingFlattened = iteratedClass.isMetaClass && metaclassHierarchiesToFlatten.find(iteratedClass.className) != metaclassHierarchiesToFlatten.end();
        bool classBeingFlattened = !iteratedClass.isMetaClass && classHierarchiesToFlatten.find(iteratedClass.className) != classHierarchiesToFlatten.end();

        if (metaClassBeingFlattened || classBeingFlattened) {
            result.flatteningRootName = iteratedClass.className;
            result.flatteningRootSuperclassLocation = iteratedClass.superclass;
            result.flatteningRootSuperclass = iteratedClass.superclassLocator();
            success = true;
            break;
        }
    }

    result.isInFlatteningHierarchy = success;

    return result;
}

/// Inline selectors from parent classes into child classes for performance
void IMPCachesBuilder::inlineSelectors(DylibState& dylib, const std::unordered_map<std::string_view, DylibState*>& dylibsByInstallName)
{
    for ( const imp_caches::Class& objcClass : dylib.inputDylib->classes ) {
        auto classIt = objcClasses.find(&objcClass);
        if ( classIt == objcClasses.end() )
            continue;
        const auto& theClass = classIt->second;

        if (!isClassInteresting(theClass)) {
            continue;
        }

        std::string_view classNameString(theClass.className);
        ClassKey key {
            .name = classNameString,
            .metaclass = theClass.isMetaClass
        };

        IMPCaches::ClassData* thisDataPtr = dylib.impCachesClassData.at(key).get();
        assert(thisDataPtr != NULL);

        __block std::set<Selector*> seenSelectors;
        for (const ClassData::Method& method : thisDataPtr->methods) {
            seenSelectors.insert(method.selector);
        };

        // Check the superclass hierarchy to see if we're in a flattened hierarchy
        // (meaning we should inline all of the selectors up to the flattening root)
        FlatteningRootLookupResult flatteningInfo = findFlatteningRoot(objcClass, objcClasses, classHierarchiesToFlatten, metaclassHierarchiesToFlatten, false);

        if (flatteningInfo.isInFlatteningHierarchy) {
            // try again and record superclasses this time
            // (maybe premature optimization, but given the small number of classes where flattening
            //  is actually happening I did not want to gather this set every time)

            flatteningInfo = findFlatteningRoot(objcClass, objcClasses, classHierarchiesToFlatten, metaclassHierarchiesToFlatten, true);

            assert(flatteningInfo.isInFlatteningHierarchy);

            thisDataPtr->flatteningRootSuperclass = flatteningInfo.flatteningRootSuperclass;
            thisDataPtr->flatteningRootName = flatteningInfo.flatteningRootName;
            thisDataPtr->flattenedSuperclasses = flatteningInfo.superclassesInFlatteningHierarchy;
        }

        // Iterate again to actually flatten/inline the selectors
        const imp_caches::Class*            superclassLocation = &objcClass;
        __block const imp_caches::Dylib*    currentDylib = dylib.inputDylib;
        bool isRootClass = false;
        bool isFlattening =  flatteningInfo.isInFlatteningHierarchy;

        while (!isRootClass) {
            const auto it = objcClasses.find(superclassLocation);
            if (it == objcClasses.end()) {
                break;
            }
            ObjCClass& iteratedClass = it->second;
            isRootClass = iteratedClass.isRootClass;

            DylibState* classDylib = dylibsByInstallName.at(currentDylib->installName);
            ClassKey keyForIteratedClass {
                .name = std::string_view(iteratedClass.className),
                .metaclass = iteratedClass.isMetaClass
            };
            auto classDataIt = classDylib->impCachesClassData.find(keyForIteratedClass);

            // We should have added this class to our data in populateMethodLists()
            assert(classDataIt != classDylib->impCachesClassData.end());

            IMPCaches::ClassData* classData = classDataIt->second.get();

            for (const ClassData::Method& m : classData->methods) {
                // If the method found in the superclass was inlined from a further superclass, we'll inline it
                // when we reach that class (otherwise the install name / class name the method is coming from will be wrong)
                if (!m.wasInlined) {
                    inlineMethodIfNeeded(thisDataPtr, m.className, m.categoryName, currentDylib->installName, m.selector->name, seenSelectors, isFlattening);
                }
            }

            currentDylib = iteratedClass.superclassDylib;
            assert(isRootClass || (currentDylib != nullptr));
            superclassLocation = iteratedClass.superclass;

            if (isFlattening && (iteratedClass.superclass == flatteningInfo.flatteningRootSuperclassLocation)) {
                // we reached the flattening root, turn flattening off
                isFlattening = false;
            }
        }
    }
}

/// Parses all the source dylibs to fill the IMP cache skeletons with all the methods we want to have there.
bool IMPCachesBuilder::parseDylibs(Diagnostics& diag) {
    std::unordered_map<std::string_view, DylibState*> dylibsByInstallName;
    
    for (DylibState& d : dylibs) {
        dylibsByInstallName.insert(std::make_pair(d.inputDylib->installName, &d));

        // Build the set of tracked classes (interesting classes + their superclasses)
        buildTrackedClasses(d);
    }

    int totalDuplicateClassCount = 0;

    for (DylibState& d : dylibs) {
        int duplicateClassCount = 0;
        // First, go through all classes and populate their method lists.
        populateMethodLists(d, &duplicateClassCount);
        totalDuplicateClassCount += duplicateClassCount;

        // Now go through all categories and attach them as well
        attachCategories(d);
    }
    
    _diagnostics.verbose("[IMP caches] Not generating caches for %d duplicate classes or children of duplicate classes\n", totalDuplicateClassCount);

    // Ensure that all the selectors will fit on 16 MB as that's the constant
    // embedded in the placement algorithm
    uint32_t totalSize = 0;
    for (const auto& [k,v] : selectors.map) {
        totalSize += v->size();
    }
    if (totalSize >= (1 << 24)) {
        diag.warning("Dropping all IMP caches ; too many selectors\n");
        return false;
    }

    for (DylibState& d : dylibs) {
        // Now that all categories are attached, handle any selector inheritance if needed
        // (Do this after category attachment so that inlined selectors don't override categories)
        
        inlineSelectors(d, dylibsByInstallName);
    }

    removeUninterestingClasses();

    unsigned count = 0;
    for (DylibState& d : dylibs) {
        count += d.impCachesClassData.size();
    }
    
    constexpr bool logAllSelectors = false;

    diag.verbose("[IMP Caches] parsed %u classes\n", count);
    for (DylibState& d : dylibs) {
        for (auto it = d.impCachesClassData.begin() ; it != d.impCachesClassData.end() ; it++) {
            auto& c = it->second;
            c->didFinishAddingMethods();
            if (logAllSelectors) {
                printf("%s\n", c->description().c_str());
                std::vector<ClassData::Method> sortedMethods(c->methods.begin(), c->methods.end());
                std::sort(sortedMethods.begin(), sortedMethods.end(), [](const ClassData::Method& a, const ClassData::Method& b) {
                    return a.selector->name < b.selector->name;
                });
                for (const ClassData::Method& m : sortedMethods) {
                    const Selector* s = m.selector;
                    printf("  %s", s->name.data());
                    if (m.categoryName != nullptr) {
                        printf("  (from %s::%s+%s)\n", m.installName.data(), m.className.data(), m.categoryName.data());
                    } else if (m.className != c->name) {
                        printf(" (from %s::%s)\n", m.installName.data(), m.className.data());
                    } else {
                        printf("\n");
                    }
                }
            }
        }
    }

    for (auto& s : selectorsToInline) {
        auto selectorsIt = selectors.map.find(s);
        if (selectorsIt == selectors.map.end()) {
            diag.warning("Requested selector to inline not found in any classes: %s\n", s.data());
            continue;
        }
        inlinedSelectors.push_back(selectors.map.at(s).get());
    }
    std::sort(inlinedSelectors.begin(), inlinedSelectors.end(), [](const Selector* a, const Selector *b) {
        return a->offset < b->offset;
    });

    return true;
}

void IMPCachesBuilder::buildClassesMap(Diagnostics& diag)
{
    const bool log = false;

    ClassSet seenClasses;

    for ( DylibState& dylib : this->dylibs ) {
        for ( const imp_caches::Class& objcClass : dylib.inputDylib->classes ) {
            std::string_view className = objcClass.name;
            if ( log )
                printf("%s: %s\n", dylib.inputDylib->installName.data(), className.data());

            bool isRootClass = objcClass.isRootClass;
            bool isMetaClass = objcClass.isMetaClass;
            if ( (objcClass.superClass != nullptr) || isRootClass) {
                objcClasses[&objcClass] = ObjCClass {
                    .superclassDylib    = objcClass.superClassDylib,
                    .metaClass          = isMetaClass ? nullptr : objcClass.metaClass,
                    .superclass         = objcClass.superClass,
                    .className          = className,
                    .isRootClass        = isRootClass,
                    .isMetaClass        = isMetaClass,
                };

                ClassKey k {
                    .name = className,
                    .metaclass = isMetaClass
                };

                auto [it, success] = seenClasses.insert(k);
                if (!success) {
                    duplicateClasses.insert(k);
                }
            }
        }

        for ( const imp_caches::Category& objcCategory : dylib.inputDylib->categories ) {
            if ( objcCategory.cls != nullptr ) {
                objcCategories[&objcCategory] = ObjCCategory {
                    .classDylib = objcCategory.classDylib,
                    .cls = objcCategory.cls
                };
            } else {
                // This happens for categories on weak classes that may be missing.
                objcCategories[&objcCategory] = ObjCCategory {
                    .classDylib = nullptr,
                    .cls = nullptr
                };
            }
        }
    }

    // Print the class hierarchy just to see that we found everything
    if (log) {
        for (const auto& [location, theClass] : objcClasses) {
            printf("%p %c%s", location, theClass.isMetaClass ? '+' : '-', theClass.className.data());
            bool isRoot = theClass.isRootClass;
            const imp_caches::Class* superclass = theClass.superclass;
            while ( !isRoot ) {
                auto it = objcClasses.find(superclass);
                // assert(it != objcClasses.end());
                if (it == objcClasses.end()) {
                    printf(": missing");
                    break;
                }
                printf(" : %c%s", it->second.isMetaClass ? '+' : '-', it->second.className.data());
                isRoot = it->second.isRootClass;
                superclass = it->second.superclass;
            }
            printf("\n");
        }
    }
}

const std::string * IMPCachesBuilder::nameAndIsMetaclassPairFromNode(const dyld3::json::Node & node, bool* metaclass) {
    const dyld3::json::Node& metaclassNode = dyld3::json::getRequiredValue(_diagnostics, node, "metaclass");
    if (_diagnostics.hasError()) return nullptr;

    if (metaclass != nullptr) *metaclass  = dyld3::json::parseRequiredInt(_diagnostics, metaclassNode) != 0;
    const dyld3::json::Node& nameNode = dyld3::json::getRequiredValue(_diagnostics, node, "name");
    if (_diagnostics.hasError()) return nullptr;

    return &(dyld3::json::parseRequiredString(_diagnostics, nameNode));
}

IMPCachesBuilder::IMPCachesBuilder(Diagnostics& diag, TimeRecorder& timeRecorder,
                                   const std::vector<imp_caches::Dylib>& inputDylibs,
                                   const dyld3::json::Node& optimizerConfiguration)
    : _diagnostics(diag), _timeRecorder(timeRecorder)
{
    // Add one DylibState for every input dylib
    this->dylibs.reserve(inputDylibs.size());
    for ( const imp_caches::Dylib& inputDylib : inputDylibs ) {
        DylibState d;
        d.inputDylib = &inputDylib;
        this->dylibs.push_back(std::move(d));
    }

    const dyld3::json::Node * version = dyld3::json::getOptionalValue(diag, optimizerConfiguration, "version");
    int64_t versionInt = (version != NULL) ? dyld3::json::parseRequiredInt(diag, *version) : 1;
    if (versionInt == 2) {
        // v2 has a single neededClasses array, with a key to know if it's a metaclass
        // or class. This lets us order them by importance so that we handle the important
        // cases first in the algorithm, while it's still easy to place things (as we process
        // more classes, constraints build up and we risk dropping difficult classes)

        const dyld3::json::Node& classes = dyld3::json::getRequiredValue(diag, optimizerConfiguration, "neededClasses");
        if (diag.hasError()) return;

        int i = 0;
        for (const dyld3::json::Node& n : classes.array) {
            bool metaclass = false;
            const std::string *name = nameAndIsMetaclassPairFromNode(n, &metaclass);
            if (name != nullptr) {
                if (metaclass) {
                    neededMetaclasses[*name] = i++;
                } else {
                    neededClasses[*name] = i++;
                }
            } else {
                // nameAndIsMetaclassPairFromNode already logs an error in this case,
                // so nothing to do here
            }
        }
    } else {
        auto metaclasses = optimizerConfiguration.map.find("neededMetaclasses");
        int i = 0;

        if (metaclasses != optimizerConfiguration.map.cend()) {
            for (const dyld3::json::Node& n : metaclasses->second.array) {
                neededMetaclasses[n.value] = i++;
            }
        }

        auto classes = optimizerConfiguration.map.find("neededClasses");

        if (classes != optimizerConfiguration.map.cend()) {
            for (const dyld3::json::Node& n : classes->second.array) {
                neededClasses[n.value] = i++;
            }
        }
    }

    auto sels = optimizerConfiguration.map.find("selectorsToInline");
    if (sels != optimizerConfiguration.map.cend()) {
        // TODO: The emitter for this isn't implemented yet
        assert(sels->second.array.empty());
        for (const dyld3::json::Node& n : sels->second.array) {
            selectorsToInline.insert(n.value);
        }
    }

    const dyld3::json::Node* classHierarchiesToFlattenNode = dyld3::json::getOptionalValue(diag, optimizerConfiguration, "flatteningRoots");
    if (classHierarchiesToFlattenNode != nullptr) {
        for (const dyld3::json::Node& n : classHierarchiesToFlattenNode->array) {
            bool metaclass = false;
            const std::string *name = nameAndIsMetaclassPairFromNode(n, &metaclass);
            if (metaclass) {
                metaclassHierarchiesToFlatten.insert(*name);
            } else {
                classHierarchiesToFlatten.insert(*name);
            }
        }
    } else {
        // For old files, we assume we should flatten OS_object, this was implied
        // before we decided to extend this set.

        metaclassHierarchiesToFlatten.insert("OS_object");
        classHierarchiesToFlatten.insert("OS_object");
    }
}

struct BacktrackingState {
    // Index into the next array that we are currently trying
    size_t currentAttemptIndex;

    // Possible placement attempts for this class
    std::vector<typename IMPCaches::ClassData::PlacementAttempt> attempts;

    // What we had to modify to attempt the current placement. This needs
    // to be reversed if we backtrack.
    std::optional<IMPCaches::ClassData::PlacementAttempt::Result> result;

    // We also need to store the state of the random number generator,
    // because when reverting to a snapshot we need to apply exactly the same
    // steps as last time.
    std::minstd_rand randomNumberGenerator;

    bool operator== (const BacktrackingState & other) const {
        // Don't test attempts, they will be the same as long as the class index is
        // the same, and we never compare states for different indices
        return (currentAttemptIndex == other.currentAttemptIndex) && (randomNumberGenerator == other.randomNumberGenerator);
    }
};

static void backtrack(std::vector<BacktrackingState>& backtrackingStack, int& numberOfDroppedClasses, std::vector<IMPCaches::ClassData*>& allClasses) {
    int i = (int)backtrackingStack.size() - 1;
    assert(i>0);
    BacktrackingState & last = backtrackingStack.back();
    if (!last.result) {
        // backtracking over a skipped class
        numberOfDroppedClasses--;
    }
    allClasses[i]->backtrack(*(last.result));
    backtrackingStack.pop_back();
};

template <typename Func>
static void forEachClassInFlatteningHierarchy(const IMPCaches::ClassData *parentClass, const std::vector<IMPCaches::ClassData*>& allClasses, const Func &callback) {
    if (!parentClass->flatteningRootSuperclass.has_value()) {
        return;
    }
    for (IMPCaches::ClassData * c : allClasses) {
        // If c has parentClass in its flattening hierarchy
        if ((c != parentClass)
            && c->flatteningRootSuperclass.has_value()
            && (*(c->flatteningRootSuperclass) == *(parentClass->flatteningRootSuperclass))
            && (c->flatteningRootName == parentClass->flatteningRootName)
            && (c->flattenedSuperclasses.find(parentClass->name) != c->flattenedSuperclasses.end())) {
            callback(c);
        }
    }
}

static void dropClass(Diagnostics& diagnostics, unsigned long& currentClassIndex, int& numberOfDroppedClasses, std::vector<BacktrackingState>& backtrackingStack, std::minstd_rand& randomNumberGenerator, std::vector<IMPCaches::ClassData*>& allClasses, const char* reason) {
    IMPCaches::ClassData* droppedClass = allClasses[currentClassIndex];

    diagnostics.verbose("%lu: dropping class %s (%s) because %s\n",
                        currentClassIndex, droppedClass->name.data(), droppedClass->isMetaclass ? "metaclass" : "class", reason);
    droppedClass->shouldGenerateImpCache = false;

    // If we are inside a flattened hierarchy, we need to also drop any classes inheriting from us,
    // as objc relies on all classes inside a flattened hierarchy having constant caches to do invalidation
    // properly.
    forEachClassInFlatteningHierarchy(droppedClass, allClasses, [&numberOfDroppedClasses, &diagnostics, currentClassIndex](IMPCaches::ClassData *c){
        // Drop it as well.
        // We could undrop them if we undrop droppedClass while backtracking or restoring
        // a snapshot, but it's not worth the effort.

        if (c->shouldGenerateImpCache) {
            numberOfDroppedClasses++;
            c->shouldGenerateImpCache = false;
            c->droppedBecauseFlatteningSuperclassWasDropped = true;
            diagnostics.verbose("%lu: also dropping %s (%s) in the same flattening hierarchy\n",
                                currentClassIndex, c->name.data(), c->isMetaclass ? "metaclass" : "class");
        }
    });

    currentClassIndex++;
    numberOfDroppedClasses++;

    BacktrackingState state = {
        .currentAttemptIndex = 0,
        .randomNumberGenerator = randomNumberGenerator
    };
    
    backtrackingStack.push_back(state);
};

static void resetToSnapshot(std::vector<BacktrackingState>& backtrackingStack, std::vector<BacktrackingState>& bestSolutionSnapshot, std::vector<IMPCaches::ClassData*>& allClasses, int& numberOfDroppedClasses) {

    // First, backtrack if needed until we reach the first different step.
    size_t firstDifferentStep = -1;
    for (size_t i = 0 ; i < backtrackingStack.size() && i < bestSolutionSnapshot.size() ; i++) {
        if (backtrackingStack[i] != bestSolutionSnapshot[i]) {
            firstDifferentStep = i;
            break;
        }
    }

    if (firstDifferentStep == (size_t)(-1)) {
        firstDifferentStep = std::min((int)backtrackingStack.size(), (int)bestSolutionSnapshot.size());
    }
    
    while (backtrackingStack.size() > firstDifferentStep) {
        backtrack(backtrackingStack, numberOfDroppedClasses, allClasses);
    }

    // Then apply the steps needed to get to the snapshot.
    if (firstDifferentStep < bestSolutionSnapshot.size()) {
        for (size_t i = (int)backtrackingStack.size() ; i < bestSolutionSnapshot.size() ; i++) {
            BacktrackingState & state = bestSolutionSnapshot[i];

            // Make a copy in order not to mutate it should we need to go back...
            std::minstd_rand stateRandomNumberGenerator = state.randomNumberGenerator;
            if (state.result) {
                assert(state.currentAttemptIndex < state.attempts.size());
                typename IMPCaches::ClassData::PlacementAttempt::Result result = allClasses[i]->applyAttempt(state.attempts[state.currentAttemptIndex], stateRandomNumberGenerator);
                assert(result.success);

                if (!allClasses[i]->droppedBecauseFlatteningSuperclassWasDropped) {
                    // shouldGenerateImpCache might have been flipped to false during backtracking
                    // we're restoring to a snapshot where we did place this class, so restore
                    // the success bit...
                    // ... unless we had decided to drop it because other classes were dropped
                    // (in that case give up and don't attempt to generate a cache for it,
                    //  but still apply the attempt above in order to set the right constraints
                    //  on each selector, which is necessary for snapshot reproducibility)

                    allClasses[i]->shouldGenerateImpCache = true;
                }
            } else {
                numberOfDroppedClasses++;
            }

            backtrackingStack.push_back(state);
        }
    }
}

/// Finds a shift and mask for each class, and start assigning the bits of the selector addresses
int IMPCachesBuilder::findShiftsAndMasks() {
    // Always seed the random number generator with 0 to get reproducibility.
    // Note: in overflow scenarios, findShiftsAndMasks can be called more than once,
    // so make sure to always use the same value when we enter this method.
    std::minstd_rand randomNumberGenerator(0);
    
    // This is a backtracking algorithm, so we need a stack to store our state
    // (It goes too deep to do it recursively)
    std::vector<BacktrackingState> backtrackingStack;

    // Index of the class we're currently looking at.
    unsigned long currentClassIndex = 0;
    
    // This lets us backtrack by more than one step, going back eg. 4 classes at a time.
    // Yes, this means we're not exploring the full solution space, but it's OK because
    // there are many solutions out there and we prefer dropping a few classes here and
    // there rather than take hours to find the perfect solution.
    unsigned long backtrackingLength = 1;

    // Indices of the attempt we had chosen for each class last time we reached the maximum
    // number of classes placed so far.
    std::vector<BacktrackingState> bestSolutionSnapshot;

#if 0
    // Debugging facilities where we store the full state corresponding
    // to bestSolutionSnapshot to make sure restoring snapshots works.
    std::vector<IMPCaches::ClassData> bestSolutionSnapshotClasses;
    std::unordered_map<std::string_view, IMPCaches::Selector> bestSolutionSnapshotSelectors;
#endif

    // Number of times we have backtracked. When this becomes too high, we go back to the
    // previous snapshot and drop the faulty class.
    unsigned long backtrackingAttempts = 0;

    // Go through all the classes and find a shift and mask for each,
    // backtracking if needed.
    std::vector<IMPCaches::ClassData*> allClasses;
    fillAllClasses(allClasses);
    
    int numberOfDroppedClasses = 0;

    while (currentClassIndex < allClasses.size()) {
        /* for (int i = 0 ; i < backtrackingStack.size() ; i++) {
            assert(backtrackingStack[i].attempts.size() > 0 || !allClasses[i]->shouldGenerateImpCache);
        } */

        assert(// Either we are adding a new state...
               (currentClassIndex == backtrackingStack.size())
               // Or we are backtracking and building on the last state recorded
               || (currentClassIndex == (backtrackingStack.size() - 1)));

        IMPCaches::ClassData* c = allClasses[currentClassIndex];

        if (!c->shouldGenerateImpCache) {
            // We have decided to drop this one before, so don't waste time.
            dropClass(_diagnostics, currentClassIndex, numberOfDroppedClasses, backtrackingStack, randomNumberGenerator, allClasses, "we have dropped it before");
            continue;
        }

        if (c->isPartOfDuplicateSet) {
            dropClass(_diagnostics, currentClassIndex, numberOfDroppedClasses, backtrackingStack, randomNumberGenerator, allClasses, "it is part of a duplicate set");
            continue;
        }

        if (currentClassIndex >= backtrackingStack.size()) {
            // We're at the top of the stack. Make a fresh state.

            BacktrackingState state;
            state.attempts = c->attempts();
            state.currentAttemptIndex = 0;
            state.randomNumberGenerator = randomNumberGenerator;
            backtrackingStack.push_back(state);
        } else {
            // We are backtracking ; don't retry the attempt we tried before, use the next one.
            backtrackingStack[currentClassIndex].currentAttemptIndex++;

            // Note that we do not reset randomNumberGenerator to state.randomNumberGenerator
            // here, because when backtracking we want to explore a different set of
            // possibilities, so let's try other placements.
        }

        assert(backtrackingStack.size() == currentClassIndex + 1);

        BacktrackingState& state = backtrackingStack[currentClassIndex];

        bool placed = false;

        // Go through all the possible placement attempts for this class.
        // If one succeeds, place the next class, and if needed we'll backtrack and try the next attempt, etc.
        // This is basically an iterative backtracking because
        // we don't want the stack to get too deep.
        std::vector<typename IMPCaches::ClassData::PlacementAttempt> & attempts = state.attempts;
        for (size_t operationIndex = state.currentAttemptIndex ; operationIndex < attempts.size() ; operationIndex++) {
            // Save the state of the random number generator so that we can save its
            // state before applying the attempt in the backtracking stack if needed.
            std::minstd_rand maybeSuccessfulRNG = randomNumberGenerator;
            typename IMPCaches::ClassData::PlacementAttempt::Result result = c->applyAttempt(attempts[operationIndex], randomNumberGenerator);
            if (result.success) {
                if (currentClassIndex % 1000 == 0) {
                    _diagnostics.verbose("[IMP Caches] Placed %lu / %lu classes\n", currentClassIndex, allClasses.size());
                }

                //fprintf(stderr, "%lu / %lu: placed %s with operation %d/%lu (%s)\n", currentClassIndex, allClasses.size(), c->description().c_str(), operationIndex, attempts.size(), attempts[operationIndex].description().c_str());
                placed = true;
                state.result = result;
                state.currentAttemptIndex = operationIndex;
                state.randomNumberGenerator = maybeSuccessfulRNG;
                break;
            }
        }

        if (placed) {
            currentClassIndex += 1;
        } else {
            // Remove the current state, which has just failed and does not matter.
            // (It was never applied).
            backtrackingStack.pop_back();

            backtrackingAttempts++;
            if (backtrackingAttempts > 10) {
                // Reset to the best snapshot and drop the next class

                resetToSnapshot(backtrackingStack, bestSolutionSnapshot, allClasses, numberOfDroppedClasses);

#if 0
// Expensive book-keeping to make sure that resetting to the snapshot worked.
                for (const auto & [k,v] : selectors.map) {
                    const IMPCaches::Selector & theoretical = bestSolutionSnapshotSelectors[k];
                    const IMPCaches::Selector & actual = *v;

                    if (theoretical != actual) {
                        fprintf(stderr, "Failed to restore snapshot of %lu classes; method %s differs, (%x, %x) vs (%x, %x)\n", bestSolutionSnapshot.size(), k.data(), theoretical.inProgressBucketIndex, theoretical.fixedBitsMask, actual.inProgressBucketIndex, actual.fixedBitsMask);
                        assert(theoretical == actual);
                    }
                }
#endif

                _diagnostics.verbose("*** SNAPSHOT: successfully reset to snapshot of size %lu\n", bestSolutionSnapshot.size());

                currentClassIndex = backtrackingStack.size();
                dropClass(_diagnostics, currentClassIndex, numberOfDroppedClasses, backtrackingStack, randomNumberGenerator, allClasses, "it's too difficult to place");

                // FIXME: we should consider resetting backtrackingLength to the value it had when we snapshotted here (the risk makes this not worth trying at this point in the release).

                backtrackingAttempts = 0;
                continue;
            } else {
                if (currentClassIndex > bestSolutionSnapshot.size()) {
                    _diagnostics.verbose("*** SNAPSHOT *** %lu / %lu (%s)\n", currentClassIndex, allClasses.size(), c->description().c_str());
                    bestSolutionSnapshot = backtrackingStack;

#if 0
                    // Make a full copy of the state so that we can debug resetting to snapshots
                    bestSolutionSnapshotClasses.clear();
                    bestSolutionSnapshotSelectors.clear();
                    for (const auto & [k,v] : selectors.map) {
                        bestSolutionSnapshotSelectors[k] = *v;
                    }
#endif
                }

                _diagnostics.verbose("%lu / %lu (%s): backtracking\n", currentClassIndex, allClasses.size(), c->description().c_str());
                assert(currentClassIndex != 0); // Backtracked all the way to the beginning, no solution

                for (unsigned long j = 0 ; j < backtrackingLength ; j++) {
                    backtrack(backtrackingStack, numberOfDroppedClasses, allClasses);
                    currentClassIndex--;
                }

                backtrackingLength = std::max(1ul,std::min(std::min(currentClassIndex, backtrackingLength * 2), 1024ul));
            }
        }
    }
    
    if (numberOfDroppedClasses > 0) {
        _diagnostics.verbose("Dropped %d classes that were too difficult to place\n", numberOfDroppedClasses);
    }
    
    return numberOfDroppedClasses;
}

void IMPCachesBuilder::fillAllClasses(std::vector<IMPCaches::ClassData*> & allClasses) {
    for (const DylibState & d : dylibs) {

        for (const auto& [key, thisClassData]: d.impCachesClassData) {
            if (thisClassData->methods.size() > 0 && thisClassData->shouldGenerateImpCache) {
                allClasses.push_back(thisClassData.get());
            }
        }
    }


    // Only include the classes for which there is actual work to do,
    // otherwise we have classes with only 1 choice which makes our
    // partial backtracking more difficult.

    std::sort(allClasses.begin(), allClasses.end(), [this](IMPCaches::ClassData* a, IMPCaches::ClassData* b) {
        int indexA = a->isMetaclass ? neededMetaclasses[a->name] : neededClasses[a->name];
        int indexB = b->isMetaclass ? neededMetaclasses[b->name] : neededClasses[b->name];

        return (indexA < indexB);
    });
}

void IMPCachesBuilder::removeUninterestingClasses() {
    // Remove any empty classes and classes for which we don't generate IMP caches now that we've inlined all selectors
    // (These classes were just used for inlining purposes)
    
    for (DylibState& d : dylibs) {
        for (auto it = d.impCachesClassData.begin() ; it != d.impCachesClassData.end() ; /* no increment here */) {
            auto& c = it->second;
            if (((c->methods.size() == 0) && !(c->flatteningRootSuperclass.has_value()))
                || !c->shouldGenerateImpCache) {
                // Remove this useless class: delete it from the selectors, and from the master class map
                // Note that it is not useless if it is in a flattening hierarchy: all classes in a flattening
                // hierarchy must have preopt caches so that objc correctly invalidates the caches on children
                // when you attach a category to one of the classes in a flattening hierarchy
                
                for (auto& m : c->methods) {
                    auto& classes = m.selector->classes;
                    classes.erase(std::remove(classes.begin(), classes.end(), c.get()), classes.end());
                }
                
                it = d.impCachesClassData.erase(it);
            } else {
                it++;
            }
        }
    }

    
    // Now remove from the selector map any selectors that are not used by any classes

    addressSpace.removeUninterestingSelectors();
    for (auto it = selectors.map.begin() ; it != selectors.map.end() ; /* no increment */ ) {
        Selector & s = *(it->second);
        
        if ((s.classes.size() == 0) && (it->first != magicSelector)) {
            it = selectors.map.erase(it);
        } else {
            it++;
        }
    }
}

void IMPCachesBuilder::fillAllMethods(std::vector<IMPCaches::Selector*> & allMethods) {
    for (auto& [name, selectorData] : selectors.map) {
        // Remove all non-interesting classes that were added only for inlining tracking.
        if (selectorData->classes.size() > 0) {
            allMethods.push_back(selectorData.get());
        }
    }
}

// Main entry point of the algorithm, chaining all the steps.
void IMPCachesBuilder::buildPerfectHashes(IMPCaches::HoleMap& holeMap, Diagnostics& diag) {
    _timeRecorder.pushTimedSection();
    int droppedClasses = findShiftsAndMasks();
    _timeRecorder.recordTime("find shifts and masks");
    
    if (droppedClasses > 0) {
        removeUninterestingClasses();
    }
    
    droppedClasses = solveGivenShiftsAndMasks();

    if (droppedClasses > 0) {
        removeUninterestingClasses();
    }

    computeLowBits(holeMap);

    _timeRecorder.recordTime("assign selector addresses");
    _timeRecorder.popTimedSection();
}

void IMPCachesBuilder::computeLowBits(IMPCaches::HoleMap& holeMap) {
    // Construct a new HoleMap, as we want the constructor to run and take account
    // of the magic selector
    holeMap = HoleMap();
    addressSpace.computeLowBits(holeMap);
}

// Shuffles selectors around to satisfy size constraints
int IMPCachesBuilder::solveGivenShiftsAndMasks() {
    std::vector<IMPCaches::ClassData*> allClasses;
    fillAllClasses(allClasses);

    int hadToIncreaseSizeCount = 0;
    int droppedClasses = 0;
    
    // Sanity check: all methods should have a fixed bits mask
    // that at least encompasses the masks of all the classes they are in.
    for (const IMPCaches::ClassData* c : allClasses) {
        for (const ClassData::Method& m : c->methods) {
            assert(((m.selector->fixedBitsMask >> c->shift) & c->mask()) == c->mask());
        }
        if (c->hadToIncreaseSize()) {
            hadToIncreaseSizeCount++;
        }
    }

    // Sanity check: all classes should have a valid shift and mask.
    for (IMPCaches::ClassData* c : allClasses) {
        assert(c->checkConsistency());
    }


    // Now that everything is placed, try to adjust placement within the
    // constraints so that we can respect alignment

    _diagnostics.verbose("[IMP Caches] Placed %lu classes, increasing hash table size for %d\n", allClasses.size(), hadToIncreaseSizeCount);

    std::vector<IMPCaches::Selector*> methodsSortedByNumberOfFixedBits;
    fillAllMethods(methodsSortedByNumberOfFixedBits);

    std::sort(methodsSortedByNumberOfFixedBits.begin(), methodsSortedByNumberOfFixedBits.end(), [](const IMPCaches::Selector* a, const IMPCaches::Selector* b) -> bool {

        // Place the methods with the greatest number of fixed bits first
        // as they will have the most constraints

        // If we have the same number of fixed bits, place the methods
        // in the largest number of classes first, as they will likely
        // have more constraints on their bits

        std::tuple<int, int, std::string_view> ta = std::make_tuple(a->numberOfSetBits(), a->classes.size(), std::string_view(a->name));
        std::tuple<int, int, std::string_view> tb = std::make_tuple(b->numberOfSetBits(), b->classes.size(), std::string_view(b->name));

        return ta > tb;
    });

    std::default_random_engine generator;

    _diagnostics.verbose("[IMP Caches] Rearranging selectors in 128-byte buckets…\n");

    IMPCaches::ConstraintSet cs;
    for (unsigned long methodIndex = 0 ; methodIndex < methodsSortedByNumberOfFixedBits.size() ; methodIndex++) {
        IMPCaches::Selector* m = methodsSortedByNumberOfFixedBits[methodIndex];
        if (addressSpace.canPlaceMethodAtIndex(m, m->inProgressBucketIndex)) {
            addressSpace.placeMethodAtIndex(m, m->inProgressBucketIndex);
        } else {
            // Try to find another address for m
            cs.clear();

#if DEBUG
            std::vector<IMPCaches::ClassData*> sortedClasses = m->classes;

            // Sort the classes so that we can always debug the same thing.
            std::sort(sortedClasses.begin(), sortedClasses.end(), [](const IMPCaches::ClassData* a, const IMPCaches::ClassData* b) {
                return *a < *b;
            });
            
            std::vector<IMPCaches::ClassData*> & classes = sortedClasses;
#else
            std::vector<IMPCaches::ClassData*> & classes = m->classes;
#endif

            bool atLeastOneConstraint = false;
            
            // Go through all the classes the method is used in and add constraints
            for (IMPCaches::ClassData* c : classes) {
                if (!c->shouldGenerateImpCache) continue;
                atLeastOneConstraint = true;
                IMPCaches::Constraint constraint = c->constraintForMethod(m);
                cs.add(constraint);
            }
            
            if (!atLeastOneConstraint) {
                // This method is only used in classes we have just dropped.
                continue;
            }
            
            auto dropClassesWithThisMethod = [this, &classes, &allClasses, &droppedClasses](){
                for (IMPCaches::ClassData* c : classes) {
                    c->shouldGenerateImpCache = false;
                    _diagnostics.verbose("Dropping class %s, selectors too difficult to place\n", c->name.data());
                    droppedClasses++;
                    forEachClassInFlatteningHierarchy(c, allClasses, [this](IMPCaches::ClassData *toDrop) {
                        if (toDrop->shouldGenerateImpCache) {
                            toDrop->shouldGenerateImpCache = false;
                            toDrop->droppedBecauseFlatteningSuperclassWasDropped = true;
                            _diagnostics.verbose("Dropping class %s in the same flattening hierarchy\n", toDrop->name.data());
                        }
                    });
                }
            };

            IMPCaches::Constraint& mergedConstraint = *(cs.mergedConstraint);

            if (mergedConstraint.allowedValues.size() == 0) {
                dropClassesWithThisMethod();
                continue;
            }

            bool foundValue = false;
            std::unordered_set<int> & allowedValues = mergedConstraint.allowedValues;
            int modulo = mergedConstraint.mask + 1;
            int multiplier = 1 << mergedConstraint.shift;
            // We want to go through:
            // [((0 + allowedValues) << shift) + k, ((modulo + allowedValues) << shift) + k, ((2*modulo + allowedValue) << shift) + k, ....] etc.
            // but we want to randomize this so that we don't completely
            // fill up the small addresses. If we do, and we end up with a
            // constraint that forces us to zero the high bits, we'll fail
            // to find room for the selector.

            // Range for the multiplier of the modulo above
            int addressesCount = std::max(((addressSpace.maximumIndex + 1) >> mergedConstraint.shift) / modulo, 1);

            // Fill "addresses" with [0, addressesCount[ so that we can shuffle it below
            std::vector<int> addresses(addressesCount);
            std::iota(addresses.begin(), addresses.end(), 0);

            for (int i = 0 ; i < addressesCount ; i++) {
                if (foundValue) {
                    break;
                }

                // Manual Fisher-Yates:
                // Pick a random element in [i, end[. Swap it with the i^th element. Repeat if the random element didn't work.
                // We don't do std::shuffle because it wastes time to shuffle the whole range if we find happiness in the beginning.
                std::uniform_int_distribution<int> distribution(i,addressesCount-1);
                int rd = distribution(generator);
                int baseAddress = addresses[rd];
                std::swap(addresses[i], addresses[rd]);

                for (int j : allowedValues) {
                    if (foundValue) {
                        break;
                    }

                    for (int k = 0 ; k < multiplier; k++) {
                        int bucketIndex = ((baseAddress * modulo + j) << mergedConstraint.shift) | k;
                        if (bucketIndex >= addressSpace.maximumIndex) {
                            continue;
                        }

                        bool canPlace = addressSpace.canPlaceMethodAtIndex(m, bucketIndex);
                        if (!canPlace) {
                            continue;
                        }

                        foundValue = true;
                        //std::cerr << methodIndex << "/" << methodsSortedByNumberOfFixedBits.size() << " Moving " << m->name << " to " << address << "\n";
                        m->inProgressBucketIndex = bucketIndex;
                        addressSpace.placeMethodAtIndex(m, bucketIndex);
                        break;
                    }
                }
            }
            
            if (!foundValue) {
                _diagnostics.verbose("Failed to place %s\n", m->name.data());
                dropClassesWithThisMethod();
            }
        }

        if (methodIndex % 1000 == 0) {
            _diagnostics.verbose("  %lu/%lu…\n", methodIndex, methodsSortedByNumberOfFixedBits.size());
        }

    }

    if (droppedClasses == 0) {
        _diagnostics.verbose("[IMP Caches] Placed all methods\n");
    } else {
        _diagnostics.verbose("[IMP Caches] Finished placing methods, dropping %d classes\n", droppedClasses);
    }

    constexpr bool log = false;
    if (log) {
        std::cerr << addressSpace << "\n";
    }
    
    return droppedClasses;
}

SelectorMap::SelectorMap() {
    std::unique_ptr<IMPCaches::Selector> magicSelectorStruct = std::make_unique<IMPCaches::Selector>();
    magicSelectorStruct->name = magicSelector.data();
    magicSelectorStruct->offset = 0;
    map[magicSelector] = std::move(magicSelectorStruct);
}

HoleMap::HoleMap() {
    addStringOfSize(magicSelector.length() + 1);
}

bool ClassData::ClassLocator::operator==(const ClassData::ClassLocator & other) {
    return (this->isMetaClass == other.isMetaClass)
        && (this->className == other.className)
        && (this->installName == other.installName);
}

bool ClassData::operator==(const ClassData & other) {
    if ( name != other.name ) {
        return false;
    }
    if (methods.size() != other.methods.size()) {
        return false;
    }
    for (unsigned i = 0 ; i < methods.size() ; i++) {
        if (*(methods[i].selector) != *(other.methods[i].selector)) {
            return false;
        }
    }

    return true;
}

const IMPCaches::ClassData::ClassLocator IMPCachesBuilder::ObjCClass::superclassLocator() const {
    return IMPCaches::ClassData::ClassLocator {
        .installName = this->superclassDylib->installName,
        .className = this->superclass->name,
        .isMetaClass = this->superclass->isMetaClass
    };
}

};

namespace imp_caches
{

Builder::Builder(const std::vector<Dylib>& dylibs, const dyld3::json::Node& objcOptimizations)
    : diags(verbose), dylibs(dylibs), objcOptimizations(objcOptimizations)
{
}

Builder::~Builder()
{
    if ( this->impCachesBuilder != nullptr )
        delete this->impCachesBuilder;
}

void Builder::buildImpCaches()
{
    impCachesBuilder = new IMPCaches::IMPCachesBuilder(this->diags, this->time, this->dylibs, this->objcOptimizations);

    // Build the class map across all dylibs (including cross-image superclass references)
    impCachesBuilder->buildClassesMap(diags);

    // Determine which methods will end up in each class's IMP cache
    bool impCachesSuccess = impCachesBuilder->parseDylibs(diags);

    // Compute perfect hash functions for IMP caches
    IMPCaches::HoleMap selectorAddressIntervals;
    if (impCachesSuccess) {
        impCachesBuilder->buildPerfectHashes(selectorAddressIntervals, diags);
    }
}

void Builder::forEachSelector(void (^handler)(std::string_view, uint32_t bufferOffset)) const
{
    if ( this->impCachesBuilder == nullptr )
        return;

    for ( const auto& stringAndSelector : this->impCachesBuilder->selectors.map )
        handler(stringAndSelector.first, (uint32_t)stringAndSelector.second->offset);
}

std::optional<imp_caches::IMPCache> Builder::getIMPCache(uint32_t dylibIndex,
                                                         std::string_view className,
                                                         bool isMetaClass)
{
    const IMPCaches::IMPCachesBuilder::DylibState& dylibState = impCachesBuilder->dylibs[dylibIndex];
    IMPCaches::ClassKey key {
        .name = className,
        .metaclass = isMetaClass
    };

    auto it = dylibState.impCachesClassData.find(key);
    if ( it == dylibState.impCachesClassData.end() )
        return { };

    const IMPCaches::ClassData* data = it->second.get();
    if ( data == nullptr )
        return { };

    if (data->mask() > 0x7ff) {
        diags.verbose("Cache for class %s is too large (mask: %#x)\n",
                     className.data(), data->mask());
        return { };
    }

    bool hasInlines = false;
    IMPCache impCache;
    std::vector<Bucket>& buckets = impCache.buckets;
    buckets.resize(data->modulo());

    for ( const IMPCaches::ClassData::Method& method : data->methods ) {
        int slot = (method.selector->inProgressBucketIndex >> data->shift) & data->mask();

        Bucket& bucket = buckets[slot];
        assert(bucket.isEmptyBucket);
        bucket.isEmptyBucket        = false;
        bucket.isInstanceMethod     = !data->isMetaclass;
        bucket.selOffset            = method.selector->offset;
        bucket.installName          = method.installName;
        bucket.className            = method.className;
        bucket.methodName           = method.selector->name;

        hasInlines |= (method.wasInlined && !method.fromFlattening);
    }

    if ( data->flatteningRootSuperclass.has_value() ) {
        FallbackClass fallbackClass = {
            .installName = data->flatteningRootSuperclass->installName,
            .className = data->flatteningRootSuperclass->className,
            .isMetaClass = data->flatteningRootSuperclass->isMetaClass
        };
        impCache.fallback_class = fallbackClass;
    }

    impCache.cache_shift = (uint32_t)(data->shift + 7);
    impCache.cache_mask = (uint32_t)data->mask();
    impCache.occupied = (uint32_t)data->methods.size();
    impCache.has_inlines = hasInlines;
    impCache.padding = 0;
    impCache.unused = 0;
    impCache.bit_one = 1; // obj-c plays HORRENDOUS games here

    return impCache;
}

Method::Method(std::string_view name)
    : name(name)
{
}

Class::Class(std::string_view name, bool isMetaClass, bool isRootClass)
    : name(name), isMetaClass(isMetaClass), isRootClass(isRootClass)
{
}

Category::Category(std::string_view name)
    : name(name)
{
}

Dylib::Dylib(std::string_view installName)
    : installName(installName)
{
}

} // namespace imp_caches
