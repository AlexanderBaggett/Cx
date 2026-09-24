struct v2 { double x, y; };
struct v3 { double x, y, z; };
__attribute__((noinline)) double dot2(struct v2 a, struct v2 b) { return a.x*b.x + a.y*b.y; }
__attribute__((noinline)) double dot3(struct v3 a, struct v3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
__attribute__((noinline)) struct v3 add3(struct v3 a, struct v3 b) { return (struct v3){a.x+b.x, a.y+b.y, a.z+b.z}; }
