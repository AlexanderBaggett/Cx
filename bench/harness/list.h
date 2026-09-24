/* Benchmark registry. Each category list is owned by its own file so the
 * lists can grow independently. Each line is B(name) for a benchmark that
 * defines `extern const struct bench bench_name`. Building with
 * -DBENCH_ONLY_<category> (make ONLY=<category>) includes one list only. */
#if defined(BENCH_ONLY_ops) || defined(BENCH_ONLY_ds) || defined(BENCH_ONLY_alg) || defined(BENCH_ONLY_kern)
#  if defined(BENCH_ONLY_ops)
#    include "list_ops.h"
#  elif defined(BENCH_ONLY_ds)
#    include "list_ds.h"
#  elif defined(BENCH_ONLY_alg)
#    include "list_alg.h"
#  else
#    include "list_kern.h"
#  endif
#else
#  include "list_ops.h"
#  include "list_ds.h"
#  include "list_alg.h"
#  include "list_kern.h"
#endif
