#ifndef ENGINE_H
#define ENGINE_H

#include "options.h"

int engine_run(const options_t* options);
void engine_interrupt(void);

#endif /* ENGINE_H */
