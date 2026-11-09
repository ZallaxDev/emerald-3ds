#include <stddef.h>
#include <string.h>
#include "lr_test.h"
#include "lr_types.h"

int main(void) {
    LR_CHECK_EQ_INT(sizeof(TrainerJourneyCore), 87);
    LR_CHECK_EQ_INT(sizeof(TrainerJourneyExtended), 221);

    /* No padding: bytes are contiguous and deterministic. */
    LR_CHECK_EQ_INT((int)(offsetof(TrainerJourneyCore, universalFacets) - offsetof(TrainerJourneyCore, hate)), 3);
    LR_CHECK_EQ_INT((int)(offsetof(TrainerJourneyCore, facetFlags) + sizeof(uint32_t)), 87);
    LR_CHECK_EQ_INT((int)(offsetof(TrainerJourneyExtended, historyTimestamps) + LR_HISTORY_SIZE), 221);

    /* Zero-initialization is the documented pre-roll state. */
    TrainerJourneyCore core;
    TrainerJourneyExtended ext;
    memset(&core, 0, sizeof(core));
    memset(&ext, 0, sizeof(ext));
    LR_CHECK_EQ_INT(core.mindRolled, 0);
    LR_CHECK_EQ_INT(core.regionFacetCount, 0);
    LR_CHECK_EQ_INT(ext.memoryCount, 0);

    /* Little-endian is the save contract. */
    uint32_t one = 1;
    unsigned char first = *((const unsigned char *)&one);
    LR_CHECK_EQ_INT(first, 1);

    return lr_test_summary("test_types");
}
