#ifndef SGF_H_
#define SGF_H_

#include "core/global.h"
#include "fastboard.h"

STRUCT_NAMED_PAIR(Loc,loc,Player,pla,Move);
STRUCT_NAMED_TRIPLE(uint8_t,x,uint8_t,y,Player,pla,MoveNoBSize);

struct SgfNode {
  map<string,vector<string>>* props;
  MoveNoBSize move;

  SgfNode();
  SgfNode(const SgfNode& other);
  ~SgfNode();

  bool hasProperty(const char* key) const;
  string getSingleProperty(const char* key) const;

  bool hasPlacements() const;
  void accumPlacements(vector<Move>& moves, int bSize) const;
  void accumMoves(vector<Move>& moves, int bSize) const;
};

struct Sgf {
  string fileName;
  vector<SgfNode*> nodes;
  vector<Sgf*> children;

  Sgf();
  ~Sgf();

  static Sgf* parse(const string& str);
  static Sgf* loadFile(const string& file);
  static vector<Sgf*> loadFiles(const vector<string>& files);

  int getBSize() const;

  void getPlacements(vector<Move>& moves, int bSize) const;
  void getMoves(vector<Move>& moves, int bSize) const;

  int depth() const;

  private:
  void getMovesHelper(vector<Move>& moves, int bSize) const;

};

struct CompactSgf {
  string fileName;
  SgfNode rootNode;
  vector<Move> placements;
  vector<Move> moves;
  int bSize;
  int depth;

  CompactSgf(const Sgf* sgf);
  ~CompactSgf();

  static CompactSgf* loadFile(const string& file);
  static vector<CompactSgf*> loadFiles(const vector<string>& files);
};

#endif
