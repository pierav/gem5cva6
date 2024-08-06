#include <cstring>
#include <unordered_map>
#include <unordered_set>

template<class Key>
struct LruSuperKey
{
  Key key;
  LruSuperKey *prev;
  LruSuperKey *next;
  LruSuperKey(Key key_,
    LruSuperKey *prev_=nullptr,
    LruSuperKey *next_=nullptr){
    key = key_;
    prev = prev_;
    next = next_;
  }
  bool operator==(const struct LruSuperKey& o) const {
    return key == o.key;
  }

};

template <class Key, class Hash = std::hash<Key> >
class lru_base
{
  protected:
  size_t max_size;
  typedef LruSuperKey<Key> SuperKey_t;
  SuperKey_t *head;
  SuperKey_t *tail;
  lru_base(size_t n) : max_size(n) {
    head = new SuperKey_t(Key()); // Create a sentinelle
    tail = new SuperKey_t(Key()); // And another one
    head->prev = tail;
    tail->next = head;
  }

  void lru_update(const SuperKey_t *csk, bool is_new){
    SuperKey_t *sk = (SuperKey_t*)csk; // To access meta data
    if (!is_new){ /* Drop element */
      assert(sk->prev);
      assert(sk->next);
      sk->prev->next = sk->next; // Sentinelle must exist
      sk->next->prev = sk->prev;
    }
    /* Append ourself at head */
    sk->prev = head->prev; // Head is a sentinelle
    sk->next = head;
    head->prev->next = sk;
    head->prev = sk;

    /* Need a drop ?*/
    if (is_overflow()){
      evict_on_overflow();
    }
  }

  virtual size_t size() = 0;
  virtual void evict(SuperKey_t *sk) = 0;

  bool is_overflow(Key *evict=nullptr){
    if (size() > max_size){ /* Drop last */
      assert(tail->next);
      if (evict){
        *evict = tail->next->key;
      }
      return true;
    }
    return false;
  }

  void evict_on_overflow(){
    assert(size()>= max_size);
    assert(tail);
    SuperKey_t *lastone = tail->next;
    assert(lastone);
    SuperKey_t *tailnext = lastone->next;
    assert(tailnext);
    evict(lastone);
    tail->next = tailnext;
  }

  class SuperHash_t
  {
    public:
      size_t operator()(const SuperKey_t& p) const {
        return Hash()(p.key);
      }
  };
};


template<class Key, class T, class Hash = std::hash<Key> >
class lru_unordered_map : public lru_base<Key, Hash>
{
  public:
  typedef typename lru_base<Key, Hash>::SuperKey_t SuperKey_t;
  typedef typename lru_base<Key, Hash>::SuperHash_t SuperHash_t;
  typedef typename std::unordered_map<SuperKey_t, T, SuperHash_t> map_t;
  typedef typename map_t::iterator BaseIt;

  map_t map;

  lru_unordered_map(size_t n) : lru_base<Key, Hash>(n) { }

  /* lru base interface */
  size_t size(){ return map.size(); }
  void evict(SuperKey_t *sk){ map.erase(*sk); }

  const Key *insert(Key key, T t){
    auto auitb = map.insert({SuperKey_t(key), t});
    const SuperKey_t *sk = &(*auitb.first).first; //
    bool is_new = auitb.second;
    this->lru_update(sk, is_new);
    return &sk->key;
  }

  bool contains(Key key){
    return map.count(SuperKey_t(key));
  }

  T& operator[](Key k){
    auto auitb = map.insert({SuperKey_t(k), T()});
    const SuperKey_t *sk = &(*auitb.first).first; //
    bool is_new = auitb.second;
    this->lru_update(sk, is_new);
    return (*auitb.first).second;
  }

  class LruSetIt : public BaseIt
  {
    typedef std::pair<Key, T> retval_t;
  public:
    LruSetIt(BaseIt it) : BaseIt(it) {}

   retval_t operator*() const {
      SuperKey_t sk = ((*((BaseIt*)this))->first);
      T &t = ((*((BaseIt*)this))->second);
      return std::pair<Key, T>(sk.key, t);
    }
    // const fake_ret_t& operator->() {
    //   SuperKey_t *res = *this;
    //   return &res->key;
    // }
  };

  LruSetIt begin() {
    return map.begin();
  }
  LruSetIt end() {
    return map.end();
  }
};

template<class Key, class Hash = std::hash<Key> >
class lru_suffix_tree
{
  size_t depth;
  size_t max_size;
  Key *keys; // Keys history

  struct IdxKey_t
  {
    Key key;
    int depth;
    IdxKey_t(Key k, int d){
      key = k;
      depth = d;
    }
    bool operator==(const IdxKey_t& o) const {
      return memcmp(this, &o, sizeof(IdxKey_t)) == 0;
    }
  };

  class IdxKeyHash
  {
    public:
      size_t operator()(const IdxKey_t& p) const {
        return Hash()(p.key) * p.depth; // a kind of hash ? :)
      }
  };

  typedef typename std::unordered_map<Key, void*, Hash> map_t;

  struct value_mid_t
  {
    /* Me */
    map_t map;
    /* Parent */
    value_mid_t *parent = nullptr;
    Key key_parent;
    value_mid_t() : map() {}
    value_mid_t(value_mid_t *parent_, Key key_parent_) : map() {
      parent = parent_;
      key_parent = key_parent_;
    }
  };

  struct value_terminal_t
  {
    /* LRU */
    value_terminal_t *next = nullptr;
    value_terminal_t *prev = nullptr;
    /* Parent */
    value_mid_t *parent;
    Key key_parent;
    value_terminal_t() {}
    value_terminal_t(value_mid_t *parent_, Key key_parent_){
      parent = parent_;
      key_parent = key_parent_;
    }
  };


  struct lru_fifo_t
  {
    value_terminal_t *head;
    value_terminal_t *tail;
    lru_fifo_t(){
      head = new value_terminal_t(); // Create a sentinelle
      tail = new value_terminal_t(); // And another one
      head->prev = tail;
      tail->next = head;
    }

    void unlink(value_terminal_t *sk){
      assert(sk->prev);
      assert(sk->next);
      sk->prev->next = sk->next; // Sentinelle must exist
      sk->next->prev = sk->prev;
    }

    void insert_head(value_terminal_t *sk){
      /* Append ourself at head */
      sk->prev = head->prev; // Head is a sentinelle
      sk->next = head;
      head->prev->next = sk;
      head->prev = sk;
    }

    value_terminal_t *evict(){
      assert(tail);
      value_terminal_t *lastone = tail->next;
      assert(lastone);
      value_terminal_t *tailnext = lastone->next;
      assert(tailnext);
      tail->next = tailnext;
      tailnext->prev = tail;
      return lastone;
    }
  } lru_fifo;

  // map_t map; // Array of map
  value_mid_t tree;
  size_t size_;

  public:
  lru_suffix_tree(size_t depth_, size_t max_size_){
    depth = depth_;
    max_size = max_size_;
    size_ = 0;
    keys = new Key[max_size];
    for (size_t i = 0; i < max_size; i++){
      keys[i] = Key();
    }
  }

  size_t size(){ return size_; }

  /* Return depth of suffix; 0 if none*/
  size_t insert(Key key){
    /* Insert in history */
    /* insert state in the fifo */
    for (size_t i = depth - 1; i > 0; i--){
      keys[i] = keys[i-1];
    }
    keys[0] = key;
    /* Fing biggest suffix */
    void *cur = &tree; // Current node
    size_t i;
    for (i = 0; i < depth; i++){ // Go depth
      if (!((value_mid_t*)cur)->map.count(keys[i])){
        break;
      }
      /* Else continue */
      cur = ((value_mid_t*)cur)->map[keys[i]];
    }
    size_t ret = i;
    if (ret == depth){ /* Remove LRU*/
      lru_fifo.unlink((value_terminal_t*)cur);
    } else { /* Insert residual part */
      size_ += 1;
      size_t ret = i;
      for (i = ret; i < depth-1; i++){
        Key key = keys[i];
        assert(cur);
        value_mid_t *next = new value_mid_t((value_mid_t*)cur, key);
        ((value_mid_t*)cur)->map[key] = next;
        cur = next;
      }
      /* Last element */
      assert(i == depth-1);
      Key key = keys[i];
      value_terminal_t *next = new value_terminal_t((value_mid_t*)cur, key);
      ((value_mid_t*)cur)->map[key] = next;
      cur = next;
    }
    lru_fifo.insert_head((value_terminal_t*)cur);

    if (size_ > max_size){
      /* Perfom eviction */
      value_terminal_t* evicted = lru_fifo.evict();
      /* Drop the path */
      evict_parent(evicted->parent, evicted->key_parent);
      delete evicted;
      size_ -= 1;
    }
    return ret;
  }

  void evict_parent(value_mid_t *e, Key k){
    if (!e){
      return;
    }
    e->map.erase(e->map.find(k)); // Unlink element
    if (e->map.size() == 0){
      // assert(e->map.size() == 0);
      evict_parent(e->parent, e->key_parent);
      delete e;
    }
  }

  void dump(){
    printf("DUMP:(%ld)\n", tree.map.size());
    dump(&tree, 0);
    /* lru_dump*/
    lru_dump();
  }

  void dump(value_mid_t *cur, size_t deep){
    for (auto e: cur->map){
      const Key *key = &e.first;
      void *value = e.second;
      for (size_t k = 0; k < deep; k++)
        printf("|");
      printf("%d\n", (int)*key);
      if (deep != depth -1)
        dump((value_mid_t*)value, deep+1);
    }
  }

  void dump_parent(value_mid_t *e){
    if (e->parent){
      assert(e->parent->map[e->key_parent] == e);
      dump_parent(e->parent);
      printf("%d-", (int)e->key_parent);
    }
  }

  void lru_dump(){
    value_terminal_t *cur = lru_fifo.tail->next;
    int i = 0;
    while (cur->next){
      printf("token[%d] : ", i++);
      assert(cur->parent);
      dump_parent(cur->parent);
      printf("%d\n", cur->key_parent);
      // printf("%d\n", cur->key_parent);
      cur = cur->next;
    }
  }
};

template <class Key, class Hash = std::hash<Key> >
class lru_unordered_set
{
  size_t max_size;

  typedef LruSuperKey<Key> SuperKey_t;
  SuperKey_t *head;
  SuperKey_t *tail;

  class SuperKeyHash_t
  {
  public:
      size_t operator()(const SuperKey_t& p) const {
        return Hash()(p.key);
      }
  };

  std::unordered_set<SuperKey_t, SuperKeyHash_t> set;

  typedef typename std::unordered_set<SuperKey_t, SuperKeyHash_t>::iterator
    BaseIt;

  class LruSetIt : public BaseIt
  {
    public:
    LruSetIt(BaseIt it) : BaseIt(it) {}
    const Key& operator*() const {
      const SuperKey_t *res = &(**((BaseIt*)this));
      return res->key;
    }
    Key& operator->() {
      SuperKey_t *res = *this;
      return &res->key;
    }
  };

  public:
  LruSetIt begin() {
    return LruSetIt(set.begin());
  }
  LruSetIt end() {
    return set.end();
  }

  lru_unordered_set(size_t n) : max_size(n) {
    head = new SuperKey_t(Key()); // Create a sentinelle
    tail = new SuperKey_t(Key()); // And another one
    head->prev = tail;
    tail->next = head;
  }

  size_t size(){
    return set.size();
  }

  bool contains(Key k){
    return set.count(SuperKey_t(k));
  }

  const Key *insert(Key k){
    // New key
    SuperKey_t sk0 = SuperKey_t(k);
    auto tuitb = set.insert(sk0);
    SuperKey_t *sk = (SuperKey_t *)&(*tuitb.first); // Drop const
    // printf("Insert key : %p : (%d)\n", sk, tuitb.second);

    if (tuitb.second){ /* Insertions : not present */
        if (is_overflow()){
            evict();
        }
    } else { /* LRU : remove ourserlf */
      assert(sk->prev);
      assert(sk->next);
      sk->prev->next = sk->next; // Sentinelle must exist
      sk->next->prev = sk->prev;
    }

    /* Append ourserf at head */
    sk->prev = head->prev; // Head is a sentinelle
    sk->next = head;
    head->prev->next = sk;
    head->prev = sk;

    // SuperKey_t *tok = tail;
    // do{
    //   printf("T=%p->", tok);
    //   tok = tok->next;
    // } while (tok->next);
    // assert(tok == head);
    // printf("\n");
    return &sk->key;
  }

  bool is_overflow(Key *evict=nullptr){
    if (set.size() > max_size){ /* Drop last */
      assert(tail->next);
      if (evict){
        *evict = tail->next->key;
      }
      return true;
    }
    return false;
  }

  void evict(){
    assert(set.size()>= max_size);
    assert(tail);
    SuperKey_t *lastone = tail->next;
    assert(lastone);
    SuperKey_t *tailnext = lastone->next;
    assert(tailnext);
    set.erase(*lastone);
    tail->next = tailnext;
  }
};


