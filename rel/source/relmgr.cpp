#include "relmgr.h"
#include "OWR.h"
#include "gc/dvd.h"
#include "gc/os.h"
#include "ttyd/memory.h"
#include "gc/OSModule.h"

#include <cstdint>
#include <cstdio>

using namespace gc::dvd;
using namespace ttyd::memory;

RelMgr relMgr;

bool RelMgr::loadRel(const char *relToLoad, int32_t heap)
{
    // Only run if a rel is not currently loaded
    if (this->relLoaded)
    {
        return false;
    }

    // Set up the filepath
    char buf[32];
    snprintf(buf, sizeof(buf), "/mod/subrels/%s.rel", relToLoad);

    // Open the rel
    DVDFileInfo fileInfo;
    if (!DVDOpen(buf, &fileInfo))
    {
        return false;
    }

    // Get the size of the rel
    uint32_t fileSize = fileInfo.length;

    // Make sure the size is a multiple of DVD_READ_SIZE, as we can only read in and at increments of DVD_READ_SIZE
    fileSize = (fileSize + DVD_READ_SIZE - 1) & ~(DVD_READ_SIZE - 1);

    // Failsafe: Make sure heap is valid
    if ((heap < 0) || (heap >= gc::os::OSAlloc_NumHeaps))
    {
        heap = HeapType::HEAP_DEFAULT;
    }

    // Allocate memory for the rel
    uint8_t *relData = reinterpret_cast<uint8_t *>(__memAlloc(heap, fileSize));

    // Read the rel from the disc
    const int32_t result = DVDReadPrio(&fileInfo, relData, fileSize, 0, 0);

    // Close the rel, as it's no longer needed
    DVDClose(&fileInfo);

    // Make sure the rel was successfully read
    if (result <= 0)
    {
        __memFree(heap, relData);
        return false;
    }

    // Get the REL's BSS size and allocate memory for it
    OSModuleInfo *relFile = reinterpret_cast<OSModuleInfo *>(relData);
    uint32_t bssSize = relFile->bssSize;

    // If bssSize is 0, then use an arbitrary size
    if (bssSize == 0)
    {
        bssSize = 1;
    }
    uint8_t *bssArea = reinterpret_cast<uint8_t *>(__memAlloc(heap, bssSize));

    // Finished loading the rel
    this->relPtr = relFile;
    this->bssPtr = bssArea;
    this->setHeap(heap);
    this->relLoaded = true;
    return true;
}

bool RelMgr::linkRel()
{
    // Only run if a rel is currently loaded
    if (!this->relLoaded)
    {
        return false;
    }

    // Link the rel file

    // Must use `Link` rather than `OSLink`, as `OSLink` is hooked with the code that calls this function, so using `OSLink`
    // would create an infinite loop
    OSModuleInfo *relFile = this->relPtr;
    if (!Link(relFile, this->bssPtr, false))
    {
        // Try to unlink to be safe
        OSUnlink(relFile);

        // Cleanup
        this->unloadRel();
        return false;
    }

    // Call the rel's prolog functon
    reinterpret_cast<void (*)()>(relFile->prologFuncOffset)();
    return true;
}

bool RelMgr::unlinkRel()
{
    // Only run if a rel is currently loaded
    if (!this->relLoaded)
    {
        return false;
    }

    // Run the rel's epilog function
    OSModuleInfo *relFile = this->relPtr;
    reinterpret_cast<void (*)()>(relFile->epilogFuncOffset)();

    // All rel functions are done, so the file can be unlinked
    if (!OSUnlink(relFile))
    {
        // Cleanup
        this->unloadRel();
        return false;
    }

    return true;
}

void RelMgr::unloadRel()
{
    // Only run if a rel is currently loaded
    if (!this->relLoaded)
    {
        return;
    }

    // Cleanup
    __memFree(this->getHeap(), this->relPtr);
    __memFree(this->getHeap(), this->bssPtr);

    this->relPtr = nullptr;
    this->bssPtr = nullptr;
    this->relLoaded = false;
}

void RelMgr::runInitRel()
{
    // Load the init rel, allocating to the ext heap to avoid fragmentation in the default heap
    if (!this->loadRel("init", HeapType::HEAP_EXT))
    {
        return;
    }

    // Link the init rel and run its prolog function
    if (!this->linkRel())
    {
        return;
    }

    // Nothing else is needed from the init rel, so it can be unlinked
    if (!this->unlinkRel())
    {
        return;
    }

    // Cleanup
    this->unloadRel();
}
