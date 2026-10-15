#include "tf_port.h"

static const char *const S_NAME[TF_SFX_COUNT] = {
    "click", "harvest", "croak", "sad", "reward",
};

const char * tf_sfx_file(int id)
{
    return (id >= 0 && id < TF_SFX_COUNT) ? S_NAME[id] : S_NAME[0];
}
