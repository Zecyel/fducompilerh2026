#include <stack>
#include <map>
#include <set>
#include <vector>
#include "loopopt.hh"
#include "flowinfo.hh"
#include "quad.hh"

// Function to find loop headers in a function and populate the LoopHeaderMap
// Complete the function!!

using namespace std;
using namespace quad;

namespace {

bool dominates(ControlFlowInfo *cfi, int dom, int block) {
    if (cfi == nullptr) return false;
    auto it = cfi->dominators.find(block);
    return it != cfi->dominators.end() && it->second.count(dom) > 0;
}

set<int> naturalLoopBody(ControlFlowInfo *cfi, int header, int latch) {
    set<int> body;
    stack<int> work;
    body.insert(header);
    if (body.insert(latch).second) work.push(latch);

    while (!work.empty()) {
        int block = work.top();
        work.pop();

        auto pred_it = cfi->predecessors.find(block);
        if (pred_it == cfi->predecessors.end()) continue;
        for (int pred : pred_it->second) {
            if (body.insert(pred).second && pred != header) {
                work.push(pred);
            }
        }
    }
    return body;
}

} // namespace

LoopHeaderMap *findLoopHeaders(QuadFuncDecl* func, FuncFlowInfo *ffi) {
    LoopHeaderMap *loopHeaderMap = new LoopHeaderMap();
    if (func == nullptr || ffi == nullptr || ffi->cfi == nullptr) {
        return loopHeaderMap;
    }
    ControlFlowInfo *cfi = ffi->cfi;

    map<int, set<int>> header_to_body;
    for (auto &succ_pair : cfi->successors) {
        int from = succ_pair.first;
        for (int to : succ_pair.second) {
            if (!dominates(cfi, to, from)) continue;
            set<int> body = naturalLoopBody(cfi, to, from);
            header_to_body[to].insert(body.begin(), body.end());
        }
    }

    set<LoopHeader*> headers;
    for (auto &entry : header_to_body) {
        headers.insert(new LoopHeader(entry.first, entry.second));
    }
    loopHeaderMap->addLoopHeader(func, headers);

    return loopHeaderMap;
}
