using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System.Collections.Generic;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    // nxnative: threads a phi through a condition that is known where the phi is used.
    //
    // Guest code reuses one register for unrelated values under complementary guards:
    //      IABS R49, R49           (an integer)
    //      @!P2 MOV R49, R53       (a float; R49 = phi(int, float) on P2)
    //      @P2  ...                (reads R49: only the integer can arrive here)
    //      @!P2 ...                (reads R49: only the float can arrive here)
    // Each guarded instruction is its own block behind a branch on the guard, so every read of the phi sits in a block (or on a phi edge) where the
    // guard's value is known, and that value names the one phi source that can have reached it. Without this the phi stays, typed by its first use,
    // and each read turns into a select that carries the other, never selected, value (bit cast) into the consumer's data flow: here the frame index of
    // a flipbook, from an integer modulo chain, into the texture coordinate output through arms no path selects.
    //
    // The rule, for v = phi(a from edge e0, b from edge e1, ...) in block M and a use of v at a point U:
    //   - an edge e implies c == k when it is the k successor edge of a branch on c with two distinct targets, or when its source block is dominated by
    //     a block whose only predecessor ends in such a branch and enters it by the k edge;
    //   - U knows c == k the same way (U's block for an operation; the incoming edges for a phi source);
    //   - if every edge into M but those of one source implies c != k, the use reads that source;
    //   - if every use that is left knows c == k (the source could not replace the phi: it is defined on the way into M, as "@P2 FADD R49, ..."),
    //     the sources that only arrive by edges implying c != k are never read, and become the constant 0.
    // It holds when c's definition strictly dominates M (c is then the same value at M's entry and at U, loops included, since M dominates U) and the
    // replacing source is a constant or defined in a block that strictly dominates M (the same value at both points, and defined before U).
    // A phi source here is tagged with the block that defines the value (Ssa.InsertPhi), not with the predecessor it arrives from, and predecessors
    // sharing a definition share one source. The source of a predecessor edge is the one whose block is nearest on the predecessor's dominator chain
    // (how Ssa.FindDefinition found it); a phi source used by another phi is replaced when every edge it stands for knows the condition.
    // The dominator tree is the one computed before SSA construction; the optimizer only ever removes edges after that (BranchElimination), which
    // can only add dominance relations, so every "dominates" taken from the tree is still true.
    static class PhiThreading
    {
        private readonly struct Fact
        {
            public readonly Operand Cond;
            public readonly bool Value;

            public Fact(Operand cond, bool value)
            {
                Cond = cond;
                Value = value;
            }
        }

        private sealed class Info
        {
            public readonly Dictionary<INode, BasicBlock> BlockOf = new();
            public readonly Dictionary<BasicBlock, int> Depth = new();
            public readonly Dictionary<BasicBlock, Fact?> EntryFacts = new();
            public readonly Dictionary<BasicBlock, BasicBlock> FactAncestors = new();
            public readonly Dictionary<PhiNode, int[]> EdgeSources = new();
        }

        public static bool RunPass(BasicBlock[] blocks)
        {
            Info info = new();
            List<(PhiNode, BasicBlock)> phis = [];

            for (int blkIndex = 0; blkIndex < blocks.Length; blkIndex++)
            {
                BasicBlock block = blocks[blkIndex];

                foreach (INode node in block.Operations)
                {
                    info.BlockOf[node] = block;

                    if (node is PhiNode phi && phi.SourcesCount >= 2 && phi.Dest != null && phi.Dest.UseOps.Count != 0)
                    {
                        phis.Add((phi, block));
                    }
                }
            }

            bool modified = false;

            foreach ((PhiNode phi, BasicBlock block) in phis)
            {
                modified |= ThreadPhi(info, phi, block);
            }

            return modified;
        }

        private static bool ThreadPhi(Info info, PhiNode phi, BasicBlock phiBlock)
        {
            int phiDepth = GetDepth(info, phiBlock);

            if (phiDepth < 0)
            {
                return false;
            }

            int[] edgeSources = EdgeSources(info, phi, phiBlock);

            if (edgeSources == null)
            {
                return false;
            }

            // What each predecessor edge implies, for the conditions branched on between the phi block's dominator and the phi block.
            // Conditions decided above that are the same on every edge and cannot tell the sources apart.
            int stopDepth = GetDepth(info, phiBlock.ImmediateDominator);
            List<Fact>[] edgeFacts = new List<Fact>[edgeSources.Length];
            List<Operand> conds = null;

            for (int index = 0; index < edgeSources.Length; index++)
            {
                List<Fact> facts = [];

                CollectEdgeFacts(info, phiBlock.Predecessors[index], phiBlock, stopDepth, facts);

                edgeFacts[index] = facts;

                foreach (Fact fact in facts)
                {
                    conds ??= [];

                    if (!conds.Contains(fact.Cond))
                    {
                        conds.Add(fact.Cond);
                    }
                }
            }

            if (conds == null)
            {
                return false;
            }

            bool modified = false;

            foreach (Operand cond in conds)
            {
                if (!StrictlyDominates(info, DefBlock(info, cond), phiBlock))
                {
                    continue;
                }

                bool?[] implied = ImpliedOnEdges(edgeFacts, cond);

                Operand whenTrue = SourceWhen(info, phi, phiBlock, edgeSources, implied, true);
                Operand whenFalse = SourceWhen(info, phi, phiBlock, edgeSources, implied, false);

                Operand dest = phi.Dest;

                foreach (INode useNode in new List<INode>(dest.UseOps))
                {
                    if (useNode == phi || !info.BlockOf.TryGetValue(useNode, out BasicBlock useBlock))
                    {
                        continue;
                    }

                    for (int index = 0; index < useNode.SourcesCount; index++)
                    {
                        if (useNode.GetSource(index) != dest)
                        {
                            continue;
                        }

                        bool? known = useNode is PhiNode usePhi
                            ? KnownOnSourceEdges(info, usePhi, useBlock, index, cond, phiDepth)
                            : KnownInBlock(info, useBlock, cond, phiDepth);

                        if (known == null)
                        {
                            continue;
                        }

                        Operand source = known.Value ? whenTrue : whenFalse;

                        if (source != null)
                        {
                            useNode.SetSource(index, source);

                            modified = true;
                        }
                    }
                }

                if (dest.UseOps.Count == 0)
                {
                    break;
                }

                modified |= DropUnreadSources(info, phi, phiBlock, edgeSources, implied, cond, phiDepth);
            }

            return modified;
        }

        // What each predecessor edge implies for the condition; null when it implies nothing (or both values: an edge that is never taken).
        private static bool?[] ImpliedOnEdges(List<Fact>[] edgeFacts, Operand cond)
        {
            bool?[] implied = new bool?[edgeFacts.Length];

            for (int index = 0; index < edgeFacts.Length; index++)
            {
                bool contradictory = false;

                foreach (Fact fact in edgeFacts[index])
                {
                    if (fact.Cond == cond)
                    {
                        contradictory |= implied[index] != null && implied[index] != fact.Value;
                        implied[index] = fact.Value;
                    }
                }

                if (contradictory)
                {
                    implied[index] = null;
                }
            }

            return implied;
        }

        // When every use of the phi knows the same value of the condition, the sources that arrive only by edges implying the other value are never
        // read: they are replaced by the constant 0, which frees what computed them from the phi's consumers.
        private static bool DropUnreadSources(Info info, PhiNode phi, BasicBlock phiBlock, int[] edgeSources, bool?[] implied, Operand cond, int phiDepth)
        {
            Operand dest = phi.Dest;
            bool? value = null;

            foreach (INode useNode in dest.UseOps)
            {
                if (useNode == phi || !info.BlockOf.TryGetValue(useNode, out BasicBlock useBlock))
                {
                    return false;
                }

                for (int index = 0; index < useNode.SourcesCount; index++)
                {
                    if (useNode.GetSource(index) != dest)
                    {
                        continue;
                    }

                    bool? known = useNode is PhiNode usePhi
                        ? KnownOnSourceEdges(info, usePhi, useBlock, index, cond, phiDepth)
                        : KnownInBlock(info, useBlock, cond, phiDepth);

                    if (known == null || (value != null && value != known))
                    {
                        return false;
                    }

                    value = known;
                }
            }

            if (value == null)
            {
                return false;
            }

            bool modified = false;

            for (int sourceIndex = 0; sourceIndex < phi.SourcesCount; sourceIndex++)
            {
                if (phi.GetSource(sourceIndex).Type == OperandType.Constant)
                {
                    continue;
                }

                bool arrives = false;
                bool read = false;

                for (int index = 0; index < edgeSources.Length; index++)
                {
                    if (edgeSources[index] == sourceIndex)
                    {
                        arrives = true;
                        read |= implied[index] == null || implied[index] == value;
                    }
                }

                if (arrives && !read)
                {
                    phi.SetSource(sourceIndex, Const(0));

                    modified = true;
                }
            }

            return modified;
        }

        // For each predecessor of the phi block (in Predecessors order), the index of the phi source that arrives from it; null when one cannot be told.
        private static int[] EdgeSources(Info info, PhiNode phi, BasicBlock phiBlock)
        {
            if (info.EdgeSources.TryGetValue(phi, out int[] edgeSources))
            {
                return edgeSources;
            }

            Dictionary<BasicBlock, int> sourceBlocks = new();

            for (int index = 0; index < phi.SourcesCount; index++)
            {
                sourceBlocks[phi.GetBlock(index)] = index;
            }

            edgeSources = phiBlock.Predecessors.Count >= 2 ? new int[phiBlock.Predecessors.Count] : null;

            for (int index = 0; edgeSources != null && index < edgeSources.Length; index++)
            {
                BasicBlock block = phiBlock.Predecessors[index];

                while (true)
                {
                    if (sourceBlocks.TryGetValue(block, out int sourceIndex))
                    {
                        edgeSources[index] = sourceIndex;

                        break;
                    }

                    BasicBlock dominator = block.ImmediateDominator;

                    // (the phi block defines the value for every edge that comes back to it: one of the sources must be tagged with it)
                    if (block == phiBlock || dominator == null || dominator == block)
                    {
                        edgeSources = null;

                        break;
                    }

                    block = dominator;
                }
            }

            info.EdgeSources.Add(phi, edgeSources);

            return edgeSources;
        }

        // The one source that can have entered the phi block while cond == value, when it may replace the phi at its uses.
        private static Operand SourceWhen(Info info, PhiNode phi, BasicBlock phiBlock, int[] edgeSources, bool?[] implied, bool value)
        {
            int sourceIndex = -1;
            int excluded = 0;

            for (int index = 0; index < edgeSources.Length; index++)
            {
                if (implied[index] != null && implied[index] != value)
                {
                    excluded++;

                    continue;
                }

                if (sourceIndex >= 0 && sourceIndex != edgeSources[index])
                {
                    return null;
                }

                sourceIndex = edgeSources[index];
            }

            if (sourceIndex < 0 || excluded == 0)
            {
                return null;
            }

            Operand source = phi.GetSource(sourceIndex);

            if (source == phi.Dest)
            {
                return null;
            }

            if (source.Type == OperandType.Constant)
            {
                return source;
            }

            if (source.Type == OperandType.LocalVariable && StrictlyDominates(info, DefBlock(info, source), phiBlock))
            {
                return source;
            }

            return null;
        }

        // The condition's value on every predecessor edge that the given source of a phi stands for, when they all know it and agree.
        private static bool? KnownOnSourceEdges(Info info, PhiNode phi, BasicBlock phiBlock, int sourceIndex, Operand cond, int stopDepth)
        {
            int[] edgeSources = EdgeSources(info, phi, phiBlock);

            if (edgeSources == null)
            {
                return null;
            }

            bool? known = null;

            for (int index = 0; index < edgeSources.Length; index++)
            {
                if (edgeSources[index] != sourceIndex)
                {
                    continue;
                }

                bool? onEdge = KnownOnEdge(info, phiBlock.Predecessors[index], phiBlock, cond, stopDepth);

                if (onEdge == null || (known != null && known != onEdge))
                {
                    return null;
                }

                known = onEdge;
            }

            return known;
        }

        private static void CollectEdgeFacts(Info info, BasicBlock from, BasicBlock to, int stopDepth, List<Fact> facts)
        {
            Fact? edgeFact = BranchFact(from, to);

            if (edgeFact != null)
            {
                facts.Add(edgeFact.Value);
            }

            for (BasicBlock block = FactAncestor(info, from); block != null && GetDepth(info, block) > stopDepth; block = FactAncestor(info, block.ImmediateDominator))
            {
                facts.Add(EntryFact(info, block).Value);
            }
        }

        private static bool? KnownOnEdge(Info info, BasicBlock from, BasicBlock to, Operand cond, int stopDepth)
        {
            Fact? edgeFact = BranchFact(from, to);

            if (edgeFact != null && edgeFact.Value.Cond == cond)
            {
                return edgeFact.Value.Value;
            }

            return KnownInBlock(info, from, cond, stopDepth);
        }

        private static bool? KnownInBlock(Info info, BasicBlock useBlock, Operand cond, int stopDepth)
        {
            for (BasicBlock block = FactAncestor(info, useBlock); block != null && GetDepth(info, block) > stopDepth; block = FactAncestor(info, block.ImmediateDominator))
            {
                Fact fact = EntryFact(info, block).Value;

                if (fact.Cond == cond)
                {
                    return fact.Value;
                }
            }

            return null;
        }

        // What taking the edge from -> to says about the condition from branches on, if it ends in a conditional branch with two distinct targets.
        private static Fact? BranchFact(BasicBlock from, BasicBlock to)
        {
            if (from.Branch == null || from.Next == null || from.Branch == from.Next)
            {
                return null;
            }

            if (from.GetLastOp() is not Operation branch || (branch.Inst != Instruction.BranchIfTrue && branch.Inst != Instruction.BranchIfFalse))
            {
                return null;
            }

            Operand cond = branch.GetSource(0);

            if (cond.Type != OperandType.LocalVariable || (to != from.Branch && to != from.Next))
            {
                return null;
            }

            return new Fact(cond, (to == from.Branch) == (branch.Inst == Instruction.BranchIfTrue));
        }

        // What being in the block says: it has a single way in, and that is one side of a conditional branch.
        private static Fact? EntryFact(Info info, BasicBlock block)
        {
            if (!info.EntryFacts.TryGetValue(block, out Fact? fact))
            {
                fact = block.Predecessors.Count == 1 ? BranchFact(block.Predecessors[0], block) : null;

                info.EntryFacts.Add(block, fact);
            }

            return fact;
        }

        // The nearest block with an entry fact among the block and its dominators.
        private static BasicBlock FactAncestor(Info info, BasicBlock block)
        {
            if (block == null)
            {
                return null;
            }

            List<BasicBlock> path = [];
            BasicBlock ancestor;

            while (true)
            {
                if (info.FactAncestors.TryGetValue(block, out ancestor))
                {
                    break;
                }

                if (EntryFact(info, block) != null)
                {
                    ancestor = block;
                    info.FactAncestors.Add(block, block);

                    break;
                }

                path.Add(block);

                BasicBlock dominator = block.ImmediateDominator;

                if (dominator == null || dominator == block)
                {
                    ancestor = null;

                    break;
                }

                block = dominator;
            }

            foreach (BasicBlock visited in path)
            {
                info.FactAncestors[visited] = ancestor;
            }

            return ancestor;
        }

        private static BasicBlock DefBlock(Info info, Operand operand)
        {
            return operand.AsgOp != null && info.BlockOf.TryGetValue(operand.AsgOp, out BasicBlock block) ? block : null;
        }

        private static bool StrictlyDominates(Info info, BasicBlock dominator, BasicBlock block)
        {
            if (dominator == null || block == null || dominator == block)
            {
                return false;
            }

            int depth = GetDepth(info, dominator);

            if (depth < 0 || GetDepth(info, block) < 0)
            {
                return false;
            }

            while (GetDepth(info, block) > depth)
            {
                block = block.ImmediateDominator;
            }

            return block == dominator;
        }

        // Depth in the dominator tree (the entry block is 0); -1 for a block that has no dominator (unreachable).
        private static int GetDepth(Info info, BasicBlock block)
        {
            if (block == null)
            {
                return -1;
            }

            List<BasicBlock> path = null;
            int depth;

            while (true)
            {
                if (info.Depth.TryGetValue(block, out depth))
                {
                    break;
                }

                BasicBlock dominator = block.ImmediateDominator;

                if (dominator == null || dominator == block)
                {
                    depth = dominator == null ? -1 : 0;
                    info.Depth.Add(block, depth);

                    break;
                }

                path ??= [];
                path.Add(block);

                block = dominator;
            }

            if (path != null)
            {
                for (int index = path.Count - 1; index >= 0; index--)
                {
                    depth = depth < 0 ? -1 : depth + 1;
                    info.Depth[path[index]] = depth;
                }
            }

            return depth;
        }
    }
}
