#include "../core/global.h"
#include "../core/sha2.h"
#include "../dataio/sgf.h"

SgfNode::SgfNode()
  :props(NULL),move(0,0,C_EMPTY)
{}
SgfNode::SgfNode(const SgfNode& other)
  :props(NULL),move(0,0,C_EMPTY)
{
  if(other.props != NULL)
    props = new map<string,vector<string>>(*(other.props));
  move = other.move;
}
SgfNode::~SgfNode()
{
  if(props != NULL)
    delete props;
}

static void propertyFail(const string& msg) {
  throw IOError(msg);
}
static void propertyFail(const char* msg) {
  propertyFail(string(msg));
}

static Loc parseSgfLoc(const string& s, int bSize) {
  if(s.length() != 2)
    propertyFail("Invalid location: " + s);

  int x = (int)s[0] - (int)'a';
  int y = (int)s[1] - (int)'a';

  if(x < 0 || x >= bSize || y < 0 || y >= bSize)
    propertyFail("Invalid location: " + s);
  return Location::getLoc(x,y,bSize);
}

static Loc parseSgfLocOrPass(const string& s, int bSize) {
  if(s.length() == 0 || s == "tt")
    return Board::PASS_LOC;
  return parseSgfLoc(s,bSize);
}

bool SgfNode::hasProperty(const char* key) const {
  if(props == NULL)
    return false;
  return contains(*props,key);
}

string SgfNode::getSingleProperty(const char* key) const {
  if(props == NULL)
    propertyFail("SGF does not contain property: " + string(key));
  if(!contains(*props,key))
    propertyFail("SGF does not contain property: " + string(key));
  const vector<string>& prop = map_get(*props,key);
  if(prop.size() != 1)
    propertyFail("SGF property is not a singleton: " + string(key));
  return prop[0];
}

bool SgfNode::hasPlacements() const {
  return props != NULL && (contains(*props,"AB") || contains(*props,"AW") || contains(*props,"AE"));
}

void SgfNode::accumPlacements(vector<Move>& moves, int bSize) const {
  if(props == NULL)
    return;
  if(contains(*props,"AB")) {
    const vector<string>& ab = map_get(*props,"AB");
    int len = ab.size();
    for(int i = 0; i<len; i++) {
      Loc loc = parseSgfLoc(ab[i],bSize);
      moves.push_back(Move(loc,P_BLACK));
    }
  }
  if(contains(*props,"AW")) {
    const vector<string>& aw = map_get(*props,"AW");
    int len = aw.size();
    for(int i = 0; i<len; i++) {
      Loc loc = parseSgfLoc(aw[i],bSize);
      moves.push_back(Move(loc,P_WHITE));
    }
  }
  if(contains(*props,"AE")) {
    const vector<string>& ae = map_get(*props,"AE");
    int len = ae.size();
    for(int i = 0; i<len; i++) {
      Loc loc = parseSgfLoc(ae[i],bSize);
      moves.push_back(Move(loc,C_EMPTY));
    }
  }
}

void SgfNode::accumMoves(vector<Move>& moves, int bSize) const {
  if(move.pla == C_BLACK) {
    if(move.x == 128 && move.y == 128)
      moves.push_back(Move(Board::PASS_LOC,move.pla));
    else {
      if(move.x >= bSize || move.y >= bSize) propertyFail("Move out of bounds: " + Global::intToString(move.x) + "," + Global::intToString(move.y));
      moves.push_back(Move(Location::getLoc(move.x,move.y,bSize),move.pla));
    }
  }
  if(props != NULL && contains(*props,"B")) {
    const vector<string>& b = map_get(*props,"B");
    int len = b.size();
    for(int i = 0; i<len; i++) {
      Loc loc = parseSgfLocOrPass(b[i],bSize);
      moves.push_back(Move(loc,P_BLACK));
    }
  }
  if(move.pla == C_WHITE) {
    if(move.x == 128 && move.y == 128)
      moves.push_back(Move(Board::PASS_LOC,move.pla));
    else {
      if(move.x >= bSize || move.y >= bSize) propertyFail("Move out of bounds: " + Global::intToString(move.x) + "," + Global::intToString(move.y));
      moves.push_back(Move(Location::getLoc(move.x,move.y,bSize),move.pla));
    }
  }
  if(props != NULL && contains(*props,"W")) {
    const vector<string>& w = map_get(*props,"W");
    int len = w.size();
    for(int i = 0; i<len; i++) {
      Loc loc = parseSgfLocOrPass(w[i],bSize);
      moves.push_back(Move(loc,P_WHITE));
    }
  }
}

Sgf::Sgf()
{}
Sgf::~Sgf() {
  for(int i = 0; i<nodes.size(); i++)
    delete nodes[i];
  for(int i = 0; i<children.size(); i++)
    delete children[i];
}


int Sgf::depth() const {
  int maxChildDepth = 0;
  for(int i = 0; i<children.size(); i++) {
    int childDepth = children[i]->depth();
    if(childDepth > maxChildDepth)
      maxChildDepth = childDepth;
  }
  return maxChildDepth + nodes.size();
}

int Sgf::getBSize() const {
  assert(nodes.size() > 0);
  int bSize;
  bool suc = Global::tryStringToInt(nodes[0]->getSingleProperty("SZ"), bSize);
  if(!suc)
    propertyFail("Could not parse board size in sgf");
  return bSize;
}

void Sgf::getPlacements(vector<Move>& moves, int bSize) const {
  moves.clear();
  assert(nodes.size() > 0);
  nodes[0]->accumPlacements(moves,bSize);
}

//Gets the longest child if the sgf has branches
void Sgf::getMoves(vector<Move>& moves, int bSize) const {
  moves.clear();
  getMovesHelper(moves,bSize);
}

void Sgf::getMovesHelper(vector<Move>& moves, int bSize) const {
  assert(nodes.size() > 0);
  for(int i = 0; i<nodes.size(); i++) {
    if(i > 0 && nodes[i]->hasPlacements())
      propertyFail("Found stone placements after the root");
    nodes[i]->accumMoves(moves,bSize);
  }

  int maxChildDepth = 0;
  Sgf* maxChild = NULL;
  for(int i = 0; i<children.size(); i++) {
    int childDepth = children[i]->depth();
    if(childDepth > maxChildDepth) {
      maxChildDepth = childDepth;
      maxChild = children[i];
    }
  }

  if(maxChild != NULL) {
    maxChild->getMovesHelper(moves,bSize);
  }
}



//PARSING---------------------------------------------------------------------

static void sgfFail(const string& msg, const string& str, int pos) {
  throw IOError(msg + " (pos " + Global::intToString(pos) + "):" + str);
}
static void sgfFail(const char* msg, const string& str, int pos) {
  sgfFail(string(msg),str,pos);
}
static void sgfFail(const string& msg, const string& str, int entryPos, int pos) {
  throw IOError(msg + " (entryPos " + Global::intToString(entryPos) + "):" + " (pos " + Global::intToString(pos) + "):" + str);
}
static void sgfFail(const char* msg, const string& str, int entryPos, int pos) {
  sgfFail(string(msg),str,entryPos,pos);
}

static char nextSgfTextChar(const string& str, int& pos) {
  if(pos >= str.length()) sgfFail("Unexpected end of str", str,pos);
  return str[pos++];
}
static char nextSgfChar(const string& str, int& pos) {
  while(true) {
    if(pos >= str.length()) sgfFail("Unexpected end of str", str,pos);
    char c = str[pos++];
    if(!Global::isWhitespace(c))
      return c;
  }
}

static string parseTextValue(const string& str, int& pos) {
  string acc;
  bool escaping = false;
  while(true) {
    char c = nextSgfTextChar(str,pos);
    if(!escaping && c == ']') {
      pos--;
      break;
    }
    if(!escaping && c == '\\') {
      escaping = true;
      continue;
    }
    if(escaping && (c == '\n' || c == '\r')) {
      while(c == '\n' || c == '\r')
        c = nextSgfTextChar(str,pos);
      pos--;
      escaping = false;
      continue;
    }
    if(c == '\t') {
      escaping = false;
      acc += ' ';
      continue;
    }

    escaping = false;
    acc += c;
  }
  return acc;
}

static bool maybeParseProperty(SgfNode* node, const string& str, int& pos) {
  int keystart = pos;
  while(Global::isAlpha(nextSgfChar(str,pos))) {}
  pos--;
  int keystop = pos;
  string key = str.substr(keystart,keystop-keystart);
  if(key.length() <= 0)
    return false;

  bool parsedAtLeastOne = false;
  while(true) {
    if(nextSgfChar(str,pos) != '[') {
      pos--;
      break;
    }
    if(node->move.pla == C_EMPTY && key == "B") {
      int bSize = 128;
      Loc loc = parseSgfLocOrPass(parseTextValue(str,pos),bSize);
      if(loc == Board::PASS_LOC)
        node->move = MoveNoBSize(128,128,P_BLACK);
      else
        node->move = MoveNoBSize((uint8_t)Location::getX(loc,bSize),(uint8_t)Location::getY(loc,bSize),P_BLACK);
    }
    else if(node->move.pla == C_EMPTY && key == "W") {
      int bSize = 128;
      Loc loc = parseSgfLocOrPass(parseTextValue(str,pos),bSize);
      if(loc == Board::PASS_LOC)
        node->move = MoveNoBSize(128,128,P_WHITE);
      else
        node->move = MoveNoBSize((uint8_t)Location::getX(loc,bSize),(uint8_t)Location::getY(loc,bSize),P_WHITE);
    }
    else {
      if(node->props == NULL)
        node->props = new map<string,vector<string>>();
      vector<string>& contents = (*(node->props))[key];
      contents.push_back(parseTextValue(str,pos));
    }
    if(nextSgfChar(str,pos) != ']') sgfFail("Expected closing bracket",str,pos);

    parsedAtLeastOne = true;
  }
  if(!parsedAtLeastOne)
    sgfFail("No property values for property " + key,str,pos);
  return true;
}

static SgfNode* maybeParseNode(const string& str, int& pos) {
  if(nextSgfChar(str,pos) != ';') {
    pos--;
    return NULL;
  }
  SgfNode* node = new SgfNode();
  try {
    while(true) {
      bool suc = maybeParseProperty(node,str,pos);
      if(!suc)
        break;
    }
  }
  catch(...) {
    delete node;
    throw;
  }
  return node;
}

static Sgf* maybeParseSgf(const string& str, int& pos) {
  if(pos >= str.length())
    return NULL;
  char c = nextSgfChar(str,pos);
  if(c != '(') {
    pos--;
    return NULL;
  }
  int entryPos = pos;
  Sgf* sgf = new Sgf();
  try {
    while(true) {
      SgfNode* node = maybeParseNode(str,pos);
      if(node == NULL)
        break;
      sgf->nodes.push_back(node);
    }
    while(true) {
      Sgf* child = maybeParseSgf(str,pos);
      if(child == NULL)
        break;
      sgf->children.push_back(child);
    }
    char c = nextSgfChar(str,pos);
    if(c != ')')
      sgfFail("Expected closing paren for sgf tree",str,entryPos,pos);
  }
  catch (...) {
    delete sgf;
    throw;
  }
  return sgf;
}


Sgf* Sgf::parse(const string& str) {
  int pos = 0;
  Sgf* sgf = maybeParseSgf(str,pos);
  uint64_t hash[4];
  SHA2::get256(str.c_str(),hash);
  sgf->hash = Hash128(hash[0],hash[1]);
  if(sgf == NULL || sgf->nodes.size() == 0)
    sgfFail("Empty sgf",str,0);
  return sgf;
}

Sgf* Sgf::loadFile(const string& file) {
  Sgf* sgf = parse(Global::readFile(file));
  if(sgf != NULL)
    sgf->fileName = file;
  return sgf;
}

vector<Sgf*> Sgf::loadFiles(const vector<string>& files) {
  vector<Sgf*> sgfs;
  try {
    for(int i = 0; i<files.size(); i++) {
      if(i % 10000 == 0)
        cout << "Loaded " << i << "/" << files.size() << " files" << endl;
      try {
        Sgf* sgf = loadFile(files[i]);
        sgfs.push_back(sgf);
      }
      catch(const IOError& e) {
        cout << "Skipping sgf file: " << files[i] << ": " << e.message << endl;
      }
    }
  }
  catch(...) {
    for(int i = 0; i<sgfs.size(); i++) {
      delete sgfs[i];
    }
    throw;
  }
  return sgfs;
}

CompactSgf::CompactSgf(const Sgf* sgf)
  :fileName(sgf->fileName),
   rootNode(*(sgf->nodes[0])),
   placements(),
   moves(),
   bSize(),
   depth()
{
  bSize = sgf->getBSize();
  depth = sgf->depth();
  hash = sgf->hash;

  sgf->getPlacements(placements, bSize);
  sgf->getMoves(moves, bSize);
}

CompactSgf::~CompactSgf() {
}

CompactSgf* CompactSgf::loadFile(const string& file) {
  Sgf* sgf = Sgf::loadFile(file);
  CompactSgf* compact = new CompactSgf(sgf);
  delete sgf;
  return compact;
}

vector<CompactSgf*> CompactSgf::loadFiles(const vector<string>& files) {
  vector<CompactSgf*> sgfs;
  try {
    for(int i = 0; i<files.size(); i++) {
      if(i % 10000 == 0)
        cout << "Loaded " << i << "/" << files.size() << " files" << endl;
      try {
        CompactSgf* sgf = loadFile(files[i]);
        sgfs.push_back(sgf);
      }
      catch(const IOError& e) {
        cout << "Skipping sgf file: " << files[i] << ": " << e.message << endl;
      }
    }
  }
  catch(...) {
    for(int i = 0; i<sgfs.size(); i++) {
      delete sgfs[i];
    }
    throw;
  }
  return sgfs;
}

