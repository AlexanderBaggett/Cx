/* Graph algorithms on random sparse graphs in CSR form: BFS, iterative DFS
 * (connected components) and Dijkstra with a binary heap. */
#include "bench.cxh"

#include <string.h>

/* ---- shared: CSR graphs ------------------------------------------------- */

/* Adjacency of vertex v is adj[off[v] .. off[v + 1]); w[] (if any) holds the
 * matching edge weights. */
struct csr {
    uint32_t nv;
    uint32_t *off;
    uint32_t *adj;
    uint32_t *w;
};

/* Directed graph: each vertex gets an out-degree in [dmin, dmax] and uniformly
 * random targets; weights (if wmax > 0) are uniform in [1, wmax]. */
static void csr_random_directed(struct csr *g, uint32_t nv, uint32_t dmin, uint32_t dmax,
                                uint32_t wmax, uint64_t seed) {
    struct rng r = { seed };
    g->nv = nv;
    g->off = (uint32_t *)bench_alloc(((size_t)nv + 1) * sizeof(uint32_t));
    g->off[0] = 0;
    for (uint32_t v = 0; v < nv; v++) g->off[v + 1] = g->off[v] + dmin + rng_below(&r, dmax - dmin + 1);
    size_t ne = g->off[nv];
    g->adj = (uint32_t *)bench_alloc(ne * sizeof(uint32_t));
    for (size_t e = 0; e < ne; e++) g->adj[e] = rng_below(&r, nv);
    g->w = NULL;
    if (wmax > 0) {
        g->w = (uint32_t *)bench_alloc(ne * sizeof(uint32_t));
        for (size_t e = 0; e < ne; e++) g->w[e] = 1 + rng_below(&r, wmax);
    }
}

/* Undirected graph with ne random edges (both directions stored). */
static void csr_random_undirected(struct csr *g, uint32_t nv, uint32_t ne, uint64_t seed) {
    struct rng r = { seed };
    uint32_t *eu = (uint32_t *)bench_alloc((size_t)ne * sizeof(uint32_t));
    uint32_t *ev = (uint32_t *)bench_alloc((size_t)ne * sizeof(uint32_t));
    uint32_t *fill = (uint32_t *)bench_zalloc(((size_t)nv + 1) * sizeof(uint32_t));
    for (uint32_t e = 0; e < ne; e++) {
        eu[e] = rng_below(&r, nv);
        ev[e] = rng_below(&r, nv);
        fill[eu[e]]++;
        fill[ev[e]]++;
    }
    g->nv = nv;
    g->off = (uint32_t *)bench_alloc(((size_t)nv + 1) * sizeof(uint32_t));
    g->off[0] = 0;
    for (uint32_t v = 0; v < nv; v++) {
        g->off[v + 1] = g->off[v] + fill[v];
        fill[v] = g->off[v];
    }
    g->adj = (uint32_t *)bench_alloc((size_t)g->off[nv] * sizeof(uint32_t));
    for (uint32_t e = 0; e < ne; e++) {
        g->adj[fill[eu[e]]] = ev[e];
        fill[eu[e]]++;
        g->adj[fill[ev[e]]] = eu[e];
        fill[ev[e]]++;
    }
    g->w = NULL;
    bench_free(eu);
    bench_free(ev);
    bench_free(fill);
}

static void csr_free(struct csr *g) {
    bench_free(g->off);
    bench_free(g->adj);
    bench_free(g->w);
}

/* marks an unvisited vertex or an empty heap slot */
#define UNSEEN UINT32_MAX

/* ---- graph_bfs: breadth-first search ------------------------------------ */

enum { BFS_V = 1 << 18, BFS_DMIN = 2, BFS_DMAX = 14, BFS_SOURCES = 3 };

/* BFS from src; dist[v] = hop count or UNSEEN. Returns the number of vertices
 * reached. */
static size_t bfs(const uint32_t *off, const uint32_t *adj, uint32_t nv, uint32_t src,
                  uint32_t *dist, uint32_t *queue) {
    for (uint32_t v = 0; v < nv; v++) dist[v] = UNSEEN;
    size_t head = 0, tail = 0;
    dist[src] = 0;
    queue[tail] = src;
    tail++;
    while (head < tail) {
        uint32_t u = queue[head];
        head++;
        uint32_t du = dist[u] + 1;
        for (uint32_t e = off[u]; e < off[u + 1]; e++) {
            uint32_t v = adj[e];
            if (dist[v] == UNSEEN) {
                dist[v] = du;
                queue[tail] = v;
                tail++;
            }
        }
    }
    return tail;
}

struct graph_bfs { struct csr g; uint32_t *dist; uint32_t *queue; };

static void *graph_bfs_setup(void) {
    struct graph_bfs *s = (struct graph_bfs *)bench_alloc(sizeof *s);
    csr_random_directed(&s->g, BFS_V, BFS_DMIN, BFS_DMAX, 0, 0x5eed0201u);
    s->dist = (uint32_t *)bench_alloc(BFS_V * sizeof(uint32_t));
    s->queue = (uint32_t *)bench_alloc(BFS_V * sizeof(uint32_t));
    return s;
}

static uint64_t graph_bfs_run(void *state) {
    const struct graph_bfs *s = (const struct graph_bfs *)state;
    uint64_t h = 0;
    for (uint32_t k = 0; k < BFS_SOURCES; k++) {
        uint32_t src = k * (BFS_V / BFS_SOURCES);
        size_t reached = bfs(s->g.off, s->g.adj, s->g.nv, src, s->dist, s->queue);
        uint64_t sum = 0;
        uint32_t maxd = 0;
        for (uint32_t v = 0; v < BFS_V; v++) {
            uint32_t d = s->dist[v];
            if (d != UNSEEN) {
                sum += d;
                if (d > maxd) maxd = d;
            }
        }
        h = mix(mix(mix(h, reached), sum), maxd);
        for (uint32_t v = 0; v < BFS_V; v += 4096) h = mix(h, s->dist[v]);
    }
    return h;
}

static void graph_bfs_teardown([[cx::escapes]] void *state) {
    struct graph_bfs *s = (struct graph_bfs *)state;
    csr_free(&s->g);
    bench_free(s->dist);
    bench_free(s->queue);
    bench_free(s);
}

extern const struct bench bench_graph_bfs = {
    "graph_bfs", "alg", "BFS from 3 sources over a random directed CSR graph (256K vertices, 2M edges)",
    graph_bfs_setup, graph_bfs_run, graph_bfs_teardown,
};

/* ---- graph_dfs: iterative DFS, connected components ---------------------- */

enum { DFS_V = 3 << 17, DFS_E = (3 << 17) + (3 << 16) };

struct dfs_result { uint32_t ncomp; uint32_t largest; uint64_t h; };

/* Label every vertex with its component (comp[v]) by depth-first search with
 * an explicit stack; pos[v] is the next edge of v to explore. */
static struct dfs_result dfs_components(const uint32_t *off, const uint32_t *adj, uint32_t nv,
                                        uint32_t *comp, uint32_t *pos, uint32_t *stack) {
    struct dfs_result res = { 0, 0, 0 };
    for (uint32_t v = 0; v < nv; v++) comp[v] = UNSEEN;
    memcpy(pos, off, (size_t)nv * sizeof(uint32_t));
    uint32_t order = 0;                 /* discovery index */
    for (uint32_t root = 0; root < nv; root++) {
        if (comp[root] != UNSEEN) continue;
        uint32_t c = res.ncomp;
        res.ncomp++;
        uint32_t size = 1;
        size_t sp = 0;
        comp[root] = c;
        stack[sp] = root;
        sp++;
        while (sp > 0) {
            uint32_t v = stack[sp - 1];
            if (pos[v] < off[v + 1]) {
                uint32_t w = adj[pos[v]];
                pos[v]++;
                if (comp[w] == UNSEEN) {
                    comp[w] = c;
                    stack[sp] = w;
                    sp++;
                    size++;
                    order++;
                    if ((order & 4095) == 0) res.h = mix(res.h, w);
                }
            } else {
                sp--;
            }
        }
        if (size > res.largest) res.largest = size;
    }
    return res;
}

struct graph_dfs { struct csr g; uint32_t *comp; uint32_t *pos; uint32_t *stack; };

static void *graph_dfs_setup(void) {
    struct graph_dfs *s = (struct graph_dfs *)bench_alloc(sizeof *s);
    csr_random_undirected(&s->g, DFS_V, DFS_E, 0x5eed0202u);
    s->comp = (uint32_t *)bench_alloc(DFS_V * sizeof(uint32_t));
    s->pos = (uint32_t *)bench_alloc(DFS_V * sizeof(uint32_t));
    s->stack = (uint32_t *)bench_alloc(DFS_V * sizeof(uint32_t));
    return s;
}

static uint64_t graph_dfs_run(void *state) {
    const struct graph_dfs *s = (const struct graph_dfs *)state;
    struct dfs_result res = dfs_components(s->g.off, s->g.adj, s->g.nv, s->comp, s->pos, s->stack);
    uint64_t h = mix(mix(res.h, res.ncomp), res.largest);
    for (uint32_t v = 0; v < DFS_V; v += 4096) h = mix(h, s->comp[v]);
    return h;
}

static void graph_dfs_teardown([[cx::escapes]] void *state) {
    struct graph_dfs *s = (struct graph_dfs *)state;
    csr_free(&s->g);
    bench_free(s->comp);
    bench_free(s->pos);
    bench_free(s->stack);
    bench_free(s);
}

extern const struct bench bench_graph_dfs = {
    "graph_dfs", "alg", "iterative DFS connected components of a random undirected graph (384K vertices, 576K edges)",
    graph_dfs_setup, graph_dfs_run, graph_dfs_teardown,
};

/* ---- graph_dijkstra: shortest paths with an indexed binary heap ---------- */

enum { DIJ_V = 1 << 17, DIJ_DMIN = 2, DIJ_DMAX = 14, DIJ_WMAX = 1000 };

/* Min-heap of vertices keyed by dist[]; where[v] is v's slot or UNSEEN. */
static void heap_up(uint32_t *heap, uint32_t *where, const uint32_t *dist, size_t i) {
    uint32_t v = heap[i];
    uint32_t key = dist[v];
    while (i > 0) {
        size_t p = (i - 1) / 2;
        uint32_t pv = heap[p];
        if (dist[pv] <= key) break;
        heap[i] = pv;
        where[pv] = (uint32_t)i;
        i = p;
    }
    heap[i] = v;
    where[v] = (uint32_t)i;
}

static void heap_down(uint32_t *heap, uint32_t *where, const uint32_t *dist, size_t n) {
    size_t i = 0;
    uint32_t v = heap[0];
    uint32_t key = dist[v];
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && dist[heap[c + 1]] < dist[heap[c]]) c++;
        uint32_t cv = heap[c];
        if (key <= dist[cv]) break;
        heap[i] = cv;
        where[cv] = (uint32_t)i;
        i = c;
    }
    heap[i] = v;
    where[v] = (uint32_t)i;
}

/* Distances from src (UNSEEN if unreachable). Weights are >= 1 and paths have
 * fewer than nv edges of weight <= DIJ_WMAX, so no distance can overflow. */
static void dijkstra(const struct csr *g, uint32_t src, uint32_t *dist, uint32_t *heap, uint32_t *where) {
    const uint32_t *off = g->off, *adj = g->adj, *wt = g->w;
    for (uint32_t v = 0; v < g->nv; v++) {
        dist[v] = UNSEEN;
        where[v] = UNSEEN;
    }
    size_t n = 0;
    dist[src] = 0;
    heap[0] = src;
    where[src] = 0;
    n = 1;
    while (n > 0) {
        uint32_t u = heap[0];
        where[u] = UNSEEN;
        n--;
        if (n > 0) {
            heap[0] = heap[n];
            heap_down(heap, where, dist, n);
        }
        uint32_t du = dist[u];
        for (uint32_t e = off[u]; e < off[u + 1]; e++) {
            uint32_t v = adj[e];
            uint32_t nd = du + wt[e];
            if (nd < dist[v]) {
                /* v is unvisited or still queued: a finished vertex has
                 * dist <= du < nd */
                size_t at;
                if (dist[v] == UNSEEN) {
                    at = n;
                    heap[n] = v;
                    n++;
                } else {
                    at = where[v];
                }
                dist[v] = nd;
                heap_up(heap, where, dist, at);
            }
        }
    }
}

struct graph_dijkstra { struct csr g; uint32_t *dist; uint32_t *heap; uint32_t *where; };

static void *graph_dijkstra_setup(void) {
    struct graph_dijkstra *s = (struct graph_dijkstra *)bench_alloc(sizeof *s);
    csr_random_directed(&s->g, DIJ_V, DIJ_DMIN, DIJ_DMAX, DIJ_WMAX, 0x5eed0203u);
    s->dist = (uint32_t *)bench_alloc(DIJ_V * sizeof(uint32_t));
    s->heap = (uint32_t *)bench_alloc(DIJ_V * sizeof(uint32_t));
    s->where = (uint32_t *)bench_alloc(DIJ_V * sizeof(uint32_t));
    return s;
}

static uint64_t graph_dijkstra_run(void *state) {
    const struct graph_dijkstra *s = (const struct graph_dijkstra *)state;
    dijkstra(&s->g, 0, s->dist, s->heap, s->where);
    uint64_t sum = 0, reached = 0, h = 0;
    uint32_t maxd = 0;
    for (uint32_t v = 0; v < DIJ_V; v++) {
        uint32_t d = s->dist[v];
        if (d != UNSEEN) {
            reached++;
            sum += d;
            if (d > maxd) maxd = d;
        }
        if ((v & 1023) == 0) h = mix(h, d);
    }
    return mix(mix(mix(h, sum), reached), maxd);
}

static void graph_dijkstra_teardown([[cx::escapes]] void *state) {
    struct graph_dijkstra *s = (struct graph_dijkstra *)state;
    csr_free(&s->g);
    bench_free(s->dist);
    bench_free(s->heap);
    bench_free(s->where);
    bench_free(s);
}

extern const struct bench bench_graph_dijkstra = {
    "graph_dijkstra", "alg", "Dijkstra with an indexed binary heap on a weighted random graph (128K vertices, 1M edges)",
    graph_dijkstra_setup, graph_dijkstra_run, graph_dijkstra_teardown,
};
