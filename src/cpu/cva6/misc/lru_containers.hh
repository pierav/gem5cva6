#include <cassert>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

template <class Key> struct LruSuperKey {
  Key key;
  LruSuperKey *prev;
  LruSuperKey *next;
  LruSuperKey(Key key_, LruSuperKey *prev_ = nullptr,
              LruSuperKey *next_ = nullptr) {
    key = key_;
    prev = prev_;
    next = next_;
  }
  LruSuperKey() : LruSuperKey(0) {}
  bool operator==(const struct LruSuperKey &o) const { return key == o.key; }
};

template <class Key, class Hash = std::hash<Key>> class lru_base {
protected:
  using SuperKey_t = LruSuperKey<Key>;
  class SuperHash_t
  {
  public:
    size_t operator()(const SuperKey_t &p) const { return Hash()(p.key); }
  };
  size_t max_size;
  SuperKey_t _mem[2];
  SuperKey_t *head;
  SuperKey_t *tail;
  lru_base(size_t n) : max_size(n) {
    head = &_mem[0]; // Create a sentinelle
    tail = &_mem[1]; // And another one
    head->prev = tail;
    tail->next = head;
  }
  virtual ~lru_base() {}

  void lru_update(const SuperKey_t *csk, bool is_new) {
    /* remove const to access meta data */
    SuperKey_t *sk = (SuperKey_t *)csk;
    if (!is_new) {                      /* Drop element */
      assert(sk->prev);
      assert(sk->next);
      sk->prev->next = sk->next; /* Sentinelle must exist */
      sk->next->prev = sk->prev;
    }
    /* Append ourself at head */
    sk->prev = head->prev; /* Head is a sentinelle */
    sk->next = head;
    head->prev->next = sk;
    head->prev = sk;

    /* Need a drop ?*/
    // SuperKey_t*cur = tail->next;
    // while (cur != head){
    //   std::cout << "-" << cur->key;
    //   cur = cur->next;
    // }
    // std::cout << std::endl;

    if (size() > max_size) { /* Evict on overflow */
      assert(tail);
      SuperKey_t *lastone = tail->next;
      assert(lastone);
      SuperKey_t *tailnext = lastone->next;
      assert(tailnext);
      evict(lastone);
      tail->next = tailnext;
    }
  }

  /* Interface */
  virtual size_t size() = 0;
  virtual void evict(SuperKey_t *sk) = 0;

};

template <class Key, class T, class Hash = std::hash<Key>>
class lru_unordered_map : public lru_base<Key, Hash>
{
  using SuperKey_t = typename lru_base<Key, Hash>::SuperKey_t;
  using SuperHash_t = typename lru_base<Key, Hash>::SuperHash_t;
  using map_t = std::unordered_map<SuperKey_t, T, SuperHash_t>;
  using BaseIt = typename map_t::iterator;
  map_t map;
public:
  class Iterator : public BaseIt
  {
      /* Wrapper around base iterator */
  public:
    Iterator(BaseIt it) : BaseIt(it) {}
    const std::pair<Key, T> operator*() {
      SuperKey_t sk = (*static_cast<BaseIt *>(this))->first;
      T &t = (*static_cast<BaseIt *>(this))->second;
      return {sk.key, t};
    }
  };
  /* API */
  lru_unordered_map(size_t n) : lru_base<Key, Hash>(n) {}
  bool contains(Key key) { return map.count(SuperKey_t(key)); }
  size_t count(Key key) { return map.count(SuperKey_t(key)); }
  size_t size() { return map.size(); }
  T &operator[](Key k) {
    auto auitb = map.insert({SuperKey_t(k), T()});
    const SuperKey_t *sk = &(*auitb.first).first; //
    bool is_new = auitb.second;
    this->lru_update(sk, is_new);
    return (*auitb.first).second;
  }
  Iterator begin() { return map.begin(); }
  Iterator end() { return map.end(); }
  /* Debug API */
  T &no_lru_access(Key k) { return map[k]; }
  /* Interface */
  private:
  void evict(SuperKey_t *sk) { map.erase(*sk); }
};

template <class Key, class Hash = std::hash<Key> >
class lru_set : public lru_base<Key, Hash>
{
  using SuperKey_t = typename lru_base<Key, Hash>::SuperKey_t;
  using SuperHash_t = typename lru_base<Key, Hash>::SuperHash_t;
  using set_t = std::unordered_set<SuperKey_t, SuperHash_t>;
  using BaseIt = typename set_t::iterator;
  set_t set;
public:
  class Iterator : public BaseIt
  {
  public:
    Iterator(BaseIt it) : BaseIt(it) {}
    Key &operator*() const {
      SuperKey_t *res = (SuperKey_t *)&(**((BaseIt *)this));
      return res->key;
    }
  };
  /* API */
  lru_set(size_t n) : lru_base<Key, Hash>(n) { }
  Iterator begin() { return set.begin(); }
  Iterator end() { return set.end(); }
  size_t size() { return set.size(); }
  bool contains(Key k) { return set.count(SuperKey_t(k)); }
  size_t count(Key k) { return set.count(SuperKey_t(k)); }
  const Key *insert(Key k) {
    auto auitb = set.insert(SuperKey_t(k));
    SuperKey_t *sk = (SuperKey_t *)&(*auitb.first); // Drop const
    this->lru_update(sk, auitb.second);
    return &sk->key;
  }
  /* Interface */
  void evict(SuperKey_t *sk) { set.erase(*sk); }
};


template <
  class Key,
  class Hash = std::hash<Key>,
  class KeyEqual = std::equal_to<Key>
> class lru_suffix_tree {
  size_t depth;
  size_t max_size;
  Key *keys; // Keys history
  /* For iterators */
  size_t cur_depth;

  using map_t = typename std::unordered_map<Key, void *, Hash, KeyEqual>;

  struct value_mid_t;

  struct base_node_t
  {
    value_mid_t *parent = nullptr;
    Key key_parent;
    base_node_t(value_mid_t *parent_ = nullptr, Key key_parent_ = Key()){
      parent = parent_;
      key_parent = key_parent_;
    }
  };

  /*
   * [ mid ] -----> [ base_node_t ]
   *    ^              |
   *    \--------------/
   *
   *
   **/
  struct value_mid_t
  {
    /* Parent : must be in the beginning */
    base_node_t base;
    /* Next */
    bool single = true;
    union
    {
      map_t *map;
      void *node = nullptr;
    } map;

    value_mid_t() : base() {}
    value_mid_t(value_mid_t *v, Key k) : base(v, k) {}
    ~value_mid_t(){
      if (!single){
        delete map.map;
      }
    }
    void insert(void *node, Key k){
      if (single){
        if (map.node == nullptr){ /* Allocate single */
          map.node = node;
          return;
        }
        base_node_t *next = (base_node_t*)map.node;
        if (KeyEqual()(next->key_parent, k)) { /* Same key */
          printf("ERROR !\n");
          exit(1);
        } else { /* Drop single and allocate map with old value*/
          single = false;
          base_node_t *next = (base_node_t*)map.node;
          assert(this == next->parent);
          // printf("old key : %d -> %p\n", next->key_parent.arr[0], next);
          map.map = new map_t(); // {{next->key_parent, next}});
          (*map.map)[next->key_parent] = (void*)next;
        }
      }
      /* Insert key in map */
      assert(map.map);
      (*map.map)[k] = node;
    }
    void *lookup(Key k){
      if (single){
        base_node_t *next = (base_node_t*)map.node;
        if (next && KeyEqual()(next->key_parent, k)){
          return next;
        }
      } else {
        if (map.map->count(k)){
          return (*map.map)[k];
        }
      }
      return nullptr;
    }
    bool erase(Key k){
      if (single){
        map.node = nullptr;
        return true;
      } else {
        map.map->erase(map.map->find(k)); // Unlink element
        return map.map->size() == 0;
      }
    }
  };

  struct value_terminal_t
  {
    /* Parent : must be in the beginning */
    base_node_t base;
    /* LRU */
    value_terminal_t *next = nullptr;
    value_terminal_t *prev = nullptr;
    value_terminal_t() {}
    value_terminal_t(value_mid_t *v, Key k) : base(v, k) {}
  };

  struct lru_fifo_t
  {
    value_terminal_t _mem[2];
    value_terminal_t *head;
    value_terminal_t *tail;
    lru_fifo_t() {
      head = &_mem[0];
      tail = &_mem[1];
      head->prev = tail;
      tail->next = head;
    }

    void drop_elem(value_terminal_t *sk) {
      assert(sk->prev);
      assert(sk->next);
      sk->prev->next = sk->next; // Sentinelle must exist
      sk->next->prev = sk->prev;
    }

    void insert_head(value_terminal_t *sk) {
      /* Append ourself at head */
      sk->prev = head->prev; // Head is a sentinelle
      sk->next = head;
      head->prev->next = sk;
      head->prev = sk;
    }

    value_terminal_t *evict() {
      assert(tail);
      value_terminal_t *lastone = tail->next;
      assert(lastone);
      value_terminal_t *tailnext = lastone->next;
      assert(tailnext);
      tail->next = tailnext;
      tailnext->prev = tail; // Care
      return lastone;
    }

    bool check(){
      value_terminal_t *cur = tail;
      printf("T:%p->", cur);
      fflush(stdout);
      while (cur != head){
        assert(cur->next->prev == cur);
        cur = cur->next;
        printf("%p->", cur);
         fflush(stdout);
      }
      printf("H:%p\n", cur);
    }
  } lru_fifo;

  value_mid_t tree;
  size_t size_;

public:
  lru_suffix_tree(size_t depth_, size_t max_size_) {
    depth = depth_;
    max_size = max_size_;
    size_ = 0;
    keys = new Key[depth]();
  }

  ~lru_suffix_tree() {
    delete[] keys;
    delete_tree();
  }

  size_t size() { return size_; }

  /* Return depth of suffix; 0 if none*/
  size_t insert(Key key) {
    /* Insert in history */
    /* insert state in the fifo */
    for (size_t i = depth - 1; i > 0; i--) {
      keys[i] = keys[i - 1];
    }
    keys[0] = key;
    /* Fing biggest suffix */
    void *cur = &tree, *next; // Current node
    size_t i;
    for (i = 0; i < depth; i++) { // Go deep
      if ((next = ((value_mid_t *)cur)->lookup(keys[i])) == nullptr){
        break;
      }
      cur = next;
    }
    /* Current depth (used by iterators) */
    cur_depth = i;
    if (cur_depth == depth) { /* Remove LRU*/
      lru_fifo.drop_elem((value_terminal_t *)cur);
    } else { /* Insert residual part */
      size_ += 1;
      for (; i < depth - 1; i++) {
        Key key = keys[i];
        assert(cur);
        next = new value_mid_t((value_mid_t *)cur, key);
        ((value_mid_t *)cur)->insert(next, key);
        cur = next;
      }
      /* Last element */
      assert(i == depth - 1);
      Key key = keys[i];
      next = new value_terminal_t((value_mid_t *)cur, key);
      ((value_mid_t *)cur)->insert(next, key);
      cur = next;
    }
    lru_fifo.insert_head((value_terminal_t *)cur);

    if (size_ > max_size) {
      value_terminal_t *evicted = lru_fifo.evict();
      /* Drop the path */
      evict_parent(evicted->base.parent, evicted->base.key_parent);
      delete evicted;
      size_ -= 1;
    }
    return cur_depth;
  }

  void evict_parent(value_mid_t *e, Key k) {
    if (!e) {
      return;
    }
    if (e->erase(k)){ /* Erase e and return number of elements */
      evict_parent(e->base.parent, e->base.key_parent);
      delete e;
    }
  }

  void dump(){
    printf("DUMP:\n");
    dump(&tree, 0);
  }

  void dump(value_mid_t *cur, size_t deep) {
    if (cur->single){
      if (cur->map.node){
        const Key *key = &((base_node_t*)cur->map.node)->key_parent;
        for (size_t k = 0; k < deep+1; k++)
          printf(">");
        printf("%d\n", (int)*key);
        if (deep != depth - 1){
          dump((value_mid_t *)cur->map.node, deep + 1);
        }
      }
    } else {
      for (auto e : *cur->map.map) {
        const Key *key = &e.first;
        void *value = e.second;
        for (size_t k = 0; k < deep+1; k++)
          printf("|");
        printf("%d\n", (int)*key);
        if (deep != depth - 1)
          dump((value_mid_t *)value, deep + 1);
      }
    }
  }

  void delete_tree() { delete_tree(&tree, 0); }
  void delete_tree(value_mid_t *cur, size_t deep) {
    if (cur->single){
      if (cur->map.node){
        if (deep != depth - 1) {
          delete_tree((value_mid_t *)cur->map.node, deep + 1);
          delete (value_mid_t *)cur->map.node;
        } else {
          delete (value_terminal_t *)cur->map.node;
        }
      }
    } else {
      for (auto e : *cur->map.map) {
        if (deep != depth - 1) {
          delete_tree((value_mid_t *)e.second, deep + 1);
          delete (value_mid_t *)e.second;
        } else {
          delete (value_terminal_t *)e.second;
        }
      }
    }
  }

  void dump_parent(value_mid_t *e) {
    if (e->parent) {
      dump_parent(e->parent);
      printf("%d-", (int)e->key_parent);
    }
  }

  /* Path iterator */
  struct iterator
  {
    lru_suffix_tree *tree;
    /* Iterator variables */
    int depth;
    int old_depth;
    iterator(lru_suffix_tree *t, size_t depth)
        : tree(t), depth(depth), old_depth(depth) {}
    bool operator==(iterator const &other) const {
      return depth == other.depth;
    }
    bool operator!=(iterator const &other) const { return !(*this == other); }
    Key &operator*() { return this->tree->keys[old_depth - depth]; }
    Key *operator->() { return &this->tree->keys[old_depth - depth]; }
    iterator &operator++() {
      assert(depth >= 0);
      depth--;
      return *this;
    }
  };

  /* Iterate over the last suffix */
  iterator begin() { return iterator(this, cur_depth); }
  iterator end() { return iterator(this, 0); }
  /* Size of the last suffix */
  size_t ssize() { return cur_depth; }
};
