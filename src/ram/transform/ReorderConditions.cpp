/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ReorderConditions.cpp
 *
 ***********************************************************************/

#include "ram/transform/ReorderConditions.h"
#include "ram/Condition.h"
#include "ram/Node.h"
#include "ram/Operation.h"
#include "ram/Program.h"
#include "ram/Statement.h"
#include "ram/analysis/Complexity.h"
#include "ram/utility/Utils.h"
#include "ram/utility/Visitor.h"
#include "souffle/utility/MiscUtil.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace souffle::ram::transform {

bool ReorderConditionsTransformer::reorderConditions(Program& program) {
    bool changed = false;
    visitDepthFirst(program, [&](const Query& query) {
        std::function<Own<Node>(Own<Node>)> filterRewriter = [&](Own<Node> node) -> Own<Node> {
            if (const Filter* filter = as<Filter>(node)) {
                const Condition* condition = &filter->getCondition();
                VecOwn<Condition> sortedConds;
                VecOwn<Condition> condList = toConjunctionList(condition);
                for (auto& cond : condList) {
                    sortedConds.emplace_back(cond->clone());
                }
                std::sort(sortedConds.begin(), sortedConds.end(), [&](Own<Condition>& a, Own<Condition>& b) {
                    return rca->getComplexity(a.get()) < rca->getComplexity(b.get());
                });

                if (!std::equal(sortedConds.begin(), sortedConds.end(), condList.begin(),
                            [](Own<Condition>& a, Own<Condition>& b) { return *a == *b; })) {
                    changed = true;
                    node = mk<Filter>(
                            Own<Condition>(toCondition(sortedConds)), souffle::clone(filter->getOperation()));
                }
            }
            node->apply(makeLambdaRamMapper(filterRewriter));
            return node;
        };
        const_cast<Query*>(&query)->apply(makeLambdaRamMapper(filterRewriter));
    });
    return changed;
}

}  // namespace souffle::ram::transform
