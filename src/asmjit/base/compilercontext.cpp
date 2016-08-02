// [AsmJit]
// Complete x86/x64 JIT and Remote Assembler for C++.
//
// [License]
// Zlib - See LICENSE.md file in the package.

// [Export]
#define ASMJIT_EXPORTS

// [Guard]
#include "../build.h"
#if !defined(ASMJIT_DISABLE_COMPILER)

// [Dependencies]
#include "../base/compilercontext_p.h"
#include "../base/utils.h"

// [Api-Begin]
#include "../apibegin.h"

namespace asmjit {

// ============================================================================
// [asmjit::RAContext - Construction / Destruction]
// ============================================================================

RAContext::RAContext(Compiler* compiler) :
  _holder(compiler->getHolder()),
  _compiler(compiler),
  _tmpAllocator(8192 - Zone::kZoneOverhead),
  _traceNode(nullptr),
  _varMapToVaListOffset(0) {

  RAContext::reset();
}
RAContext::~RAContext() {}

// ============================================================================
// [asmjit::RAContext - Reset]
// ============================================================================

void RAContext::reset(bool releaseMemory) {
  _tmpAllocator.reset(releaseMemory);

  _func = nullptr;
  _start = nullptr;
  _end = nullptr;
  _extraBlock = nullptr;
  _stop = nullptr;

  _unreachableList.reset();
  _returningList.reset();
  _jccList.reset();
  _contextVd.reset(releaseMemory);

  _memVarCells = nullptr;
  _memStackCells = nullptr;

  _mem1ByteVarsUsed = 0;
  _mem2ByteVarsUsed = 0;
  _mem4ByteVarsUsed = 0;
  _mem8ByteVarsUsed = 0;
  _mem16ByteVarsUsed = 0;
  _mem32ByteVarsUsed = 0;
  _mem64ByteVarsUsed = 0;
  _memStackCellsUsed = 0;

  _memMaxAlign = 0;
  _memVarTotal = 0;
  _memStackTotal = 0;
  _memAllTotal = 0;
  _annotationLength = 12;

  _state = nullptr;
}

// ============================================================================
// [asmjit::RAContext - Mem]
// ============================================================================

static ASMJIT_INLINE uint32_t BaseContext_getDefaultAlignment(uint32_t size) {
  if (size > 32)
    return 64;
  else if (size > 16)
    return 32;
  else if (size > 8)
    return 16;
  else if (size > 4)
    return 8;
  else if (size > 2)
    return 4;
  else if (size > 1)
    return 2;
  else
    return 1;
}

RACell* RAContext::_newVarCell(VirtReg* vreg) {
  ASMJIT_ASSERT(vreg->_memCell == nullptr);

  RACell* cell;
  uint32_t size = vreg->getSize();

  if (vreg->isStack()) {
    cell = _newStackCell(size, vreg->getAlignment());
    if (!cell) return nullptr;
  }
  else {
    cell = static_cast<RACell*>(_tmpAllocator.alloc(sizeof(RACell)));
    if (!cell) goto _NoMemory;

    cell->next = _memVarCells;
    cell->offset = 0;
    cell->size = size;
    cell->alignment = size;

    _memVarCells = cell;
    _memMaxAlign = Utils::iMax<uint32_t>(_memMaxAlign, size);
    _memVarTotal += size;

    switch (size) {
      case  1: _mem1ByteVarsUsed++ ; break;
      case  2: _mem2ByteVarsUsed++ ; break;
      case  4: _mem4ByteVarsUsed++ ; break;
      case  8: _mem8ByteVarsUsed++ ; break;
      case 16: _mem16ByteVarsUsed++; break;
      case 32: _mem32ByteVarsUsed++; break;
      case 64: _mem64ByteVarsUsed++; break;

      default:
        ASMJIT_NOT_REACHED();
    }
  }

  vreg->_memCell = cell;
  return cell;

_NoMemory:
  _compiler->setLastError(DebugUtils::errored(kErrorNoHeapMemory));
  return nullptr;
}

RACell* RAContext::_newStackCell(uint32_t size, uint32_t alignment) {
  RACell* cell = static_cast<RACell*>(_tmpAllocator.alloc(sizeof(RACell)));
  if (!cell) goto _NoMemory;

  if (alignment == 0)
    alignment = BaseContext_getDefaultAlignment(size);

  if (alignment > 64)
    alignment = 64;

  ASMJIT_ASSERT(Utils::isPowerOf2(alignment));
  size = Utils::alignTo<uint32_t>(size, alignment);

  // Insert it sorted according to the alignment and size.
  {
    RACell** pPrev = &_memStackCells;
    RACell* cur = *pPrev;

    while (cur && ((cur->alignment > alignment) || (cur->alignment == alignment && cur->size > size))) {
      pPrev = &cur->next;
      cur = *pPrev;
    }

    cell->next = cur;
    cell->offset = 0;
    cell->size = size;
    cell->alignment = alignment;

    *pPrev = cell;
    _memStackCellsUsed++;

    _memMaxAlign = Utils::iMax<uint32_t>(_memMaxAlign, alignment);
    _memStackTotal += size;
  }

  return cell;

_NoMemory:
  _compiler->setLastError(DebugUtils::errored(kErrorNoHeapMemory));
  return nullptr;
}

Error RAContext::resolveCellOffsets() {
  RACell* varCell = _memVarCells;
  RACell* stackCell = _memStackCells;

  uint32_t stackAlignment = 0;
  if (stackCell) stackAlignment = stackCell->alignment;

  uint32_t pos64 = 0;
  uint32_t pos32 = pos64 + _mem64ByteVarsUsed * 64;
  uint32_t pos16 = pos32 + _mem32ByteVarsUsed * 32;
  uint32_t pos8  = pos16 + _mem16ByteVarsUsed * 16;
  uint32_t pos4  = pos8  + _mem8ByteVarsUsed  * 8 ;
  uint32_t pos2  = pos4  + _mem4ByteVarsUsed  * 4 ;
  uint32_t pos1  = pos2  + _mem2ByteVarsUsed  * 2 ;

  uint32_t stackPos = pos1 + _mem1ByteVarsUsed;

  uint32_t gapAlignment = stackAlignment;
  uint32_t gapSize = 0;

  // TODO: Not used!
  if (gapAlignment)
    Utils::alignDiff(stackPos, gapAlignment);
  stackPos += gapSize;

  uint32_t gapPos = stackPos;
  uint32_t allTotal = stackPos;

  // Vars - Allocated according to alignment/width.
  while (varCell) {
    uint32_t size = varCell->size;
    uint32_t offset = 0;

    switch (size) {
      case  1: offset = pos1 ; pos1  += 1 ; break;
      case  2: offset = pos2 ; pos2  += 2 ; break;
      case  4: offset = pos4 ; pos4  += 4 ; break;
      case  8: offset = pos8 ; pos8  += 8 ; break;
      case 16: offset = pos16; pos16 += 16; break;
      case 32: offset = pos32; pos32 += 32; break;
      case 64: offset = pos64; pos64 += 64; break;

      default:
        ASMJIT_NOT_REACHED();
    }

    varCell->offset = static_cast<int32_t>(offset);
    varCell = varCell->next;
  }

  // Stack - Allocated according to alignment/width.
  while (stackCell) {
    uint32_t size = stackCell->size;
    uint32_t alignment = stackCell->alignment;
    uint32_t offset;

    // Try to fill the gap between variables/stack first.
    if (size <= gapSize && alignment <= gapAlignment) {
      offset = gapPos;

      gapSize -= size;
      gapPos -= size;

      if (alignment < gapAlignment)
        gapAlignment = alignment;
    }
    else {
      offset = stackPos;

      stackPos += size;
      allTotal += size;
    }

    stackCell->offset = offset;
    stackCell = stackCell->next;
  }

  _memAllTotal = allTotal;
  return kErrorOk;
}

// ============================================================================
// [asmjit::RAContext - RemoveUnreachableCode]
// ============================================================================

Error RAContext::removeUnreachableCode() {
  Compiler* compiler = getCompiler();

  PodList<AsmNode*>::Link* link = _unreachableList.getFirst();
  AsmNode* stop = getStop();

  while (link) {
    AsmNode* node = link->getValue();
    if (node && node->getPrev() && node != stop) {
      // Locate all unreachable nodes.
      AsmNode* first = node;
      do {
        if (node->hasWorkData())
          break;
        node = node->getNext();
      } while (node != stop);

      // Remove unreachable nodes that are neither informative nor directives.
      if (node != first) {
        AsmNode* end = node;
        node = first;

        // NOTE: The strategy is as follows:
        // 1. The algorithm removes everything until it finds a first label.
        // 2. After the first label is found it removes only removable nodes.
        bool removeEverything = true;
        do {
          AsmNode* next = node->getNext();
          bool remove = node->isRemovable();

          if (!remove) {
            if (node->isLabel())
              removeEverything = false;
            remove = removeEverything;
          }

          if (remove) {
            ASMJIT_TSEC({
              this->_traceNode(this, node, "[REMOVED UNREACHABLE] ");
            });
            compiler->removeNode(node);
          }

          node = next;
        } while (node != end);
      }
    }

    link = link->getNext();
  }

  return kErrorOk;
}

// ============================================================================
// [asmjit::RAContext - Liveness Analysis]
// ============================================================================

//! \internal
struct LivenessTarget {
  LivenessTarget* prev;  //!< Previous target.
  AsmLabel* node;        //!< Target node.
  AsmJump* from;         //!< Jumped from.
};

Error RAContext::livenessAnalysis() {
  uint32_t bLen = static_cast<uint32_t>(
    ((_contextVd.getLength() + BitArray::kEntityBits - 1) / BitArray::kEntityBits));

  // No variables.
  if (bLen == 0)
    return kErrorOk;

  AsmFunc* func = getFunc();
  AsmJump* from = nullptr;

  LivenessTarget* ltCur = nullptr;
  LivenessTarget* ltUnused = nullptr;

  PodList<AsmNode*>::Link* retPtr = _returningList.getFirst();
  ASMJIT_ASSERT(retPtr != nullptr);

  AsmNode* node = retPtr->getValue();
  RAData* wd;

  size_t varMapToVaListOffset = _varMapToVaListOffset;
  BitArray* bCur = newBits(bLen);
  if (!bCur) goto NoMem;

  // Allocate bits for code visited first time.
Visit:
  for (;;) {
    wd = node->getWorkData<RAData>();
    if (wd->liveness) {
      if (bCur->_addBitsDelSource(wd->liveness, bCur, bLen))
        goto Patch;
      else
        goto Done;
    }

    BitArray* bTmp = copyBits(bCur, bLen);
    if (!bTmp) goto NoMem;

    wd = node->getWorkData<RAData>();
    wd->liveness = bTmp;

    uint32_t tiedTotal = wd->tiedTotal;
    TiedReg* tiedArray = reinterpret_cast<TiedReg*>(((uint8_t*)wd) + varMapToVaListOffset);

    for (uint32_t i = 0; i < tiedTotal; i++) {
      TiedReg* tied = &tiedArray[i];
      VirtReg* vreg = tied->vreg;

      uint32_t flags = tied->flags;
      uint32_t localId = vreg->getLocalId();

      if ((flags & TiedReg::kWAll) && !(flags & TiedReg::kRAll)) {
        // Write-Only.
        bTmp->setBit(localId);
        bCur->delBit(localId);
      }
      else {
        // Read-Only or Read/Write.
        bTmp->setBit(localId);
        bCur->setBit(localId);
      }
    }

    if (node->getType() == AsmNode::kNodeLabel)
      goto Target;

    if (node == func)
      goto Done;

    ASMJIT_ASSERT(node->getPrev());
    node = node->getPrev();
  }

  // Patch already generated liveness bits.
Patch:
  for (;;) {
    ASMJIT_ASSERT(node->hasWorkData());
    ASMJIT_ASSERT(node->getWorkData<RAData>()->liveness != nullptr);

    BitArray* bNode = node->getWorkData<RAData>()->liveness;
    if (!bNode->_addBitsDelSource(bCur, bLen)) goto Done;
    if (node->getType() == AsmNode::kNodeLabel) goto Target;

    if (node == func) goto Done;
    node = node->getPrev();
  }

Target:
  if (static_cast<AsmLabel*>(node)->getNumRefs() != 0) {
    // Push a new LivenessTarget onto the stack if needed.
    if (!ltCur || ltCur->node != node) {
      // Allocate a new LivenessTarget object (from pool or zone).
      LivenessTarget* ltTmp = ltUnused;

      if (ltTmp) {
        ltUnused = ltUnused->prev;
      }
      else {
        ltTmp = _tmpAllocator.allocT<LivenessTarget>(
          sizeof(LivenessTarget) - sizeof(BitArray) + bLen * sizeof(uintptr_t));
        if (!ltTmp) goto NoMem;
      }

      // Initialize and make current - ltTmp->from will be set later on.
      ltTmp->prev = ltCur;
      ltTmp->node = static_cast<AsmLabel*>(node);
      ltCur = ltTmp;

      from = static_cast<AsmLabel*>(node)->getFrom();
      ASMJIT_ASSERT(from != nullptr);
    }
    else {
      from = ltCur->from;
      goto JumpNext;
    }

    // Visit/Patch.
    do {
      ltCur->from = from;
      bCur->copyBits(node->getWorkData<RAData>()->liveness, bLen);

      if (!from->getWorkData<RAData>()->liveness) {
        node = from;
        goto Visit;
      }

      // Issue #25: Moved 'JumpNext' here since it's important to patch
      // code again if there are more live variables than before.
JumpNext:
      if (bCur->delBits(from->getWorkData<RAData>()->liveness, bLen)) {
        node = from;
        goto Patch;
      }

      from = from->getJumpNext();
    } while (from);

    // Pop the current LivenessTarget from the stack.
    {
      LivenessTarget* ltTmp = ltCur;
      ltCur = ltCur->prev;
      ltTmp->prev = ltUnused;
      ltUnused = ltTmp;
    }
  }

  bCur->copyBits(node->getWorkData<RAData>()->liveness, bLen);
  node = node->getPrev();
  if (node->isJmp() || !node->hasWorkData()) goto Done;

  wd = node->getWorkData<RAData>();
  if (!wd->liveness) goto Visit;
  if (bCur->delBits(wd->liveness, bLen)) goto Patch;

Done:
  if (ltCur) {
    node = ltCur->node;
    from = ltCur->from;

    goto JumpNext;
  }

  retPtr = retPtr->getNext();
  if (retPtr) {
    node = retPtr->getValue();
    goto Visit;
  }

  return kErrorOk;

NoMem:
  return setLastError(DebugUtils::errored(kErrorNoHeapMemory));
}

// ============================================================================
// [asmjit::RAContext - Annotate]
// ============================================================================

Error RAContext::formatInlineComment(StringBuilder& dst, AsmNode* node) {
#if !defined(ASMJIT_DISABLE_LOGGING)
  RAData* wd = node->getWorkData<RAData>();

  if (node->hasInlineComment())
    dst.appendString(node->getInlineComment());

  if (wd && wd->liveness) {
    if (dst.getLength() < _annotationLength)
      dst.appendChars(' ', _annotationLength - dst.getLength());

    uint32_t vdCount = static_cast<uint32_t>(_contextVd.getLength());
    size_t offset = dst.getLength() + 1;

    dst.appendChar('[');
    dst.appendChars(' ', vdCount);
    dst.appendChar(']');
    BitArray* liveness = wd->liveness;

    uint32_t i;
    for (i = 0; i < vdCount; i++) {
      if (liveness->getBit(i))
        dst.getData()[offset + i] = '.';
    }

    uint32_t tiedTotal = wd->tiedTotal;
    TiedReg* tiedArray = reinterpret_cast<TiedReg*>(((uint8_t*)wd) + _varMapToVaListOffset);

    for (i = 0; i < tiedTotal; i++) {
      TiedReg* tied = &tiedArray[i];
      VirtReg* vreg = tied->vreg;
      uint32_t flags = tied->flags;

      char c = 'u';
      if ( (flags & TiedReg::kRAll) && !(flags & TiedReg::kWAll)) c = 'r';
      if (!(flags & TiedReg::kRAll) &&  (flags & TiedReg::kWAll)) c = 'w';
      if ( (flags & TiedReg::kRAll) &&  (flags & TiedReg::kWAll)) c = 'x';
      // Uppercase if unused.
      if ( (flags & TiedReg::kUnuse)) c -= 'a' - 'A';

      ASMJIT_ASSERT(offset + vreg->getLocalId() < dst.getLength());
      dst._data[offset + vreg->getLocalId()] = c;
    }
  }
#endif // !ASMJIT_DISABLE_LOGGING

  return kErrorOk;
}

// ============================================================================
// [asmjit::RAContext - Cleanup]
// ============================================================================

void RAContext::cleanup() {
  VirtReg** virtArray = _contextVd.getData();
  size_t virtCount = _contextVd.getLength();

  for (size_t i = 0; i < virtCount; i++) {
    VirtReg* vreg = virtArray[i];
    vreg->resetLocalId();
    vreg->resetPhysId();
  }

  _contextVd.reset(false);
  _extraBlock = nullptr;
}

// ============================================================================
// [asmjit::RAContext - CompileFunc]
// ============================================================================

Error RAContext::compile(AsmFunc* func) {
  AsmNode* end = func->getEnd();
  AsmNode* stop = end->getNext();

  _func = func;
  _stop = stop;
  _extraBlock = end;

  ASMJIT_PROPAGATE(fetch());
  ASMJIT_PROPAGATE(removeUnreachableCode());
  ASMJIT_PROPAGATE(livenessAnalysis());

  Compiler* compiler = getCompiler();

#if !defined(ASMJIT_DISABLE_LOGGING)
  if (compiler->getHolder()->hasLogger())
    ASMJIT_PROPAGATE(annotate());
#endif // !ASMJIT_DISABLE_LOGGING

  ASMJIT_PROPAGATE(translate());

  // We alter the compiler cursor, because it doesn't make sense to reference
  // it after compilation - some nodes may disappear and it's forbidden to add
  // new code after the compilation is done.
  compiler->_setCursor(nullptr);

  return kErrorOk;
}

} // asmjit namespace

// [Api-End]
#include "../apiend.h"

// [Guard]
#endif // !ASMJIT_DISABLE_COMPILER
