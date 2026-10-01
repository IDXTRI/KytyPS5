#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"

#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

void RemoveIdentities(const BlockList& blocks) {
	// Each identity's uses move to its argument in program order, as before; its own entry in the
	// argument's use list is dropped for all identities at once at the end. Removing it one by
	// one searched and shifted lists that grow with every identity folded into the same value.
	std::unordered_set<const Inst*> removed;
	std::vector<Inst*>              touched;
	for (auto* block: blocks) {
		auto& instructions = block->Instructions();
		for (auto inst = instructions.begin(); inst != instructions.end();) {
			if (inst->GetOpcode() != ValueOpcode::Identity) {
				inst++;
				continue;
			}
			const auto replacement = inst->Arg(0);
			inst->ReplaceUsesForRemoval(replacement, removed, touched);
			removed.insert(&*inst);
			inst = instructions.erase(inst);
		}
	}
	Inst::DropRemovedUses(touched, removed);
}

void EliminateDeadCode(const BlockList& blocks) {
	bool changed;
	do {
		changed = false;
		for (auto block = blocks.rbegin(); block != blocks.rend(); block++) {
			auto& instructions = (*block)->Instructions();
			auto  inst         = instructions.end();
			while (inst != instructions.begin()) {
				--inst;
				if (inst->HasUses() || inst->MayHaveSideEffects()) {
					continue;
				}
				inst->Invalidate();
				inst    = instructions.erase(inst);
				changed = true;
			}
		}
	} while (changed);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
