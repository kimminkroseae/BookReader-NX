#ifndef WOOK_READER_NATSORT_H
#define WOOK_READER_NATSORT_H

#include <cctype>
#include <string>

// Natural, case-insensitive ordering: "page2" < "page10".
static inline bool natural_less(const std::string &a, const std::string &b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    bool ad = isdigit((unsigned char)a[i]), bd = isdigit((unsigned char)b[j]);
    if (ad && bd) {
      size_t ai = i, bi = j;
      while (ai < a.size() && a[ai] == '0') ai++;
      while (bi < b.size() && b[bi] == '0') bi++;
      size_t ae = ai, be = bi;
      while (ae < a.size() && isdigit((unsigned char)a[ae])) ae++;
      while (be < b.size() && isdigit((unsigned char)b[be])) be++;
      size_t al = ae - ai, bl = be - bi;
      if (al != bl) return al < bl;
      int c = a.compare(ai, al, b, bi, bl);
      if (c != 0) return c < 0;
      // equal value: fewer leading zeros first, keeps ordering strict
      if ((ae - i) != (be - j)) return (ae - i) < (be - j);
      i = ae;
      j = be;
    } else {
      int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[j]);
      if (ca != cb) return ca < cb;
      i++;
      j++;
    }
  }
  if (a.size() - i != b.size() - j) return a.size() - i < b.size() - j;
  return a < b;  // total order tie-break (case)
}

struct NaturalLess {
  bool operator()(const std::string &a, const std::string &b) const {
    return natural_less(a, b);
  }
};

#endif
