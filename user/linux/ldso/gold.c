/* Original CapMix golden DSO: exercise real RELATIVE/GLOB_DAT fixups. */
static long local_value=7;
long gold_value=42;
long *gold_relative=&local_value;
long gold_answer(void){return *gold_relative+gold_value;}
