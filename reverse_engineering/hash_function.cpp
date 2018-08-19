/* 
 * This file has helper functions to find hash function which is responsible for
 * partitioning in hardware.
 * So reverse engineering happens in following steps:
 *
 * 1) Generate a pair of addresses to test.
 * 2) Test if pair of address lie on same partition.
 * 3) Collect many such pair of addresses.
 * 4) Using brute force, find all hash functions which fit.
 * 5) Repeat till all addresses bits are accounted for.
 *
 * XXX: Currently we only support XOR based hash functions (XORing of physical
 * address bits)
 */
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include <hash_function.h>

/* Maximum physical address bits currently is 64 */
#define MAX_NUM_INDEX        64

typedef struct solution {

    int indexes[MAX_NUM_INDEX];
    int depth;

} solution_t;

typedef struct hash_context {
    int min_bit;
    int max_bit;
    
    uintptr_t start_addr;               /* Range of permissible addresses */
    uintptr_t end_addr;

    uintptr_t unexplored_bits;          /* Bit mask of unexplored bits */
    uintptr_t global_unexplored_bits;   /* Across multiple base address */

    int cur_bit_to_explore;             /* Highest bit which is being explored */

    bool is_valid_pair;
    uintptr_t test_addr;                /* Pair of address to check */
    uintptr_t base_addr;

    std::vector<std::pair <uintptr_t, uintptr_t>> keys;        /* Keys with same hash */
    std::vector<solution_t> solutions;  /* Valid solutions */

} hash_context_t;

static void hash_reduce(hash_context_t *ctx);

static void print_solution(const solution_t &s)
{
    int i;

    for (i = 0; i < s.depth - 1; i++) {
        printf("Bit(%d) ^ ", s.indexes[i]);
    }
    
    if (s.depth > 0)
        printf("Bit(%d)\n", s.indexes[s.depth - 1]);
}


/* Compare equivalence of two solutions */
static int are_solutions_same(const solution_t &a, const solution_t &b)
{
    int i;

    if (a.depth != b.depth)
        return 0;

    for (i = 0; i < a.depth; i++) {
        if (a.indexes[i] != b.indexes[i])
            return 0;
    }

    return 1;
}

/* 
 * Add new bit in a solution.
 * Returns < 0 if can't insert
 */
static int insert_bit_in_solutions(solution_t &s, int bit)
{
    if (s.depth == MAX_NUM_INDEX)
        return -1;

    /* All bits are in ascending order */
    if (s.indexes[s.depth - 1] >= bit)
        return -1;

    s.indexes[s.depth] = bit;
    s.depth++;

    return 0;
}

/* 
 * Create arbitary hash function. Should keep calling this function till
 * it returns negative. In that case, size should be incremented
 */
static int permute_hypothesis(int *array, int size, int min_val, int max_val, int isfirst)
{
    int i, start_index;

    assert(size >= 1);

    if (isfirst) {
        for (i = 0; i < size; i++) {
            array[i] = min_val + i;
        }

        assert(array[size - 1] <= max_val);
        return 0;
    }


    if (array[size - 1] < max_val) {
        array[size - 1]++;
        return 0;
    }

    for (start_index = size - 2; start_index >= 0; start_index--) {
        int valid = 1;
        array[start_index]++;
        for (int i = start_index + 1; i < size; i++) {
            array[i] = array[i - 1] + 1;
            if (array[i] > max_val) {
                valid = 0;
                break;
            }
        }

        if (valid)
            break;
    }

    if (start_index < 0)
        return -1;
    
   return 0;
}

/* Combine two solutions */
static void xor_solutions(solution_t &s1, solution_t &s2, solution_t &r)
{
    int i = 0, j = 0, k = 0;

    memset(&r, 0, sizeof(solution_t));

    while (i < s1.depth || j < s2.depth) {
        if (j == s2.depth) {
            r.indexes[k] = s1.indexes[i];
            i++;
            k++;
        } else if (i == s1.depth) {
            r.indexes[k] = s2.indexes[j];
            j++;
            k++;
        } else if (s1.indexes[i] < s2.indexes[j]) {
            r.indexes[k] = s1.indexes[i];
            i++;
            k++;
        } else if (s1.indexes[i] > s2.indexes[j]) {
            r.indexes[k] = s2.indexes[j];
            j++;
            k++;
        } else {
            assert(s1.indexes[i] == s2.indexes[j]);
            i++;
            j++;
        }
        assert(k < MAX_NUM_INDEX);
    }

    r.depth = k;
}

/* Add solution and permutation of solutions with prior solutions */
static void add_solution_with_permuations(std::vector<solution_t> &perm_sarray, solution_t &new_s)
{
    int i;
    size_t old_num_solutions = perm_sarray.size();

    perm_sarray.push_back(new_s);

    for (i = 0; i < old_num_solutions; i++) {
        solution_t permuted;

        xor_solutions(perm_sarray[i], new_s, permuted);
        perm_sarray.push_back(permuted);
    }
}

/* Returns the partition number according to the key and hypothesis */
static int get_partition_num(uintptr_t key, const solution_t &s)
{
    int i;
    int partition = 0;
    
    assert(s.depth > 0);

    for (i = 0; i < s.depth; i++) {
        partition = partition ^ ((key >> s.indexes[i]) & 0x1);
    }

    return partition;
}


/* Checks if a solution is valid for all the keys */
static bool is_solution_correct(std::vector<std::pair <uintptr_t, uintptr_t>> &keys, const solution_t &s)
{
    size_t i;
    
    assert(s.depth >= 1);

    for (i = 0; i < keys.size(); i++) {

        int partition1 = get_partition_num(keys[i].first, s);
        int partition2 = get_partition_num(keys[i].second, s);

        if (partition1 != partition2)
            return false;
    }

    return true;
}

/* 
 * Find all the hash functions with which all the keys fit (i.e. lie on same
 * partition).
 * Return the number of solutions found.
 */
static int find_new_solutions(std::vector<std::pair <uintptr_t, uintptr_t>> &keys, int min_bit, int max_bit, 
        std::vector<solution_t> &solutions)
{
    int depth;
    int solutions_found = 0;
    bool ret;

    assert(solutions.size() == 0);

    for (depth = 1; depth <= max_bit - min_bit + 1; depth++) {
    
        int isFirst = 1;

        while (1) {
            solution_t s;
            s.depth = depth;

            if (permute_hypothesis(s.indexes, s.depth, min_bit, max_bit, isFirst) < 0)
                break;
            
            ret = is_solution_correct(keys, s);
            if (ret) {
                solutions.push_back(s);
                solutions_found++;
            }
            
            isFirst = 0;
        }
    }

    return solutions_found;
}

/* 
 * Checks if all keys fit with all the solutions.
 * Returns the number of solutions removed.
 */
static int remove_incorrect_solutions(hash_context_t *ctx)
{
	int i;
    int solutions_removed = 0;
    std::vector<solution_t>::iterator s;

    for (s = ctx->solutions.begin(); s != ctx->solutions.end();) {

        if (!is_solution_correct(ctx->keys, *s)) {
            solutions_removed++;
            s = ctx->solutions.erase(s);
        } else {
            ++s;
        }
    }

    return solutions_removed;
}

/* 
 * Checks if the number of solutions seems correct or are they more than 
 * they need to be.
 */
static bool are_unique_solutions_found(size_t num_solutions, int min_bit, int max_bit)
{
    /* 
    * Max number of solutions can be only be (max_bit - min_bit - 1)
    * Since n solutions give 2^n partitions
    */
    return (num_solutions <= (max_bit - min_bit + 1));
}

/* 
 * Finds solutions based on keys found.
 * Returns true if found all solutions.
 * Returns false if not sure if all solutions found.
 */
static bool try_find_all_solutions(hash_context_t *ctx)
{
    /* Do we have atleast a pair of keys to compare */
    if (ctx->keys.size() != 0) {

        /* Some solutions already exist? */
        if (ctx->solutions.size() > 0) {
            if (remove_incorrect_solutions(ctx) == 0) {

                hash_reduce(ctx);

                /* 
                 * Max number of solutions can be only be (max_bit - min_bit - 1)
                 * Since n solutions give 2^n partitions
                 */
                if (are_unique_solutions_found(ctx->solutions.size(), ctx->max_bit,
                            ctx->min_bit))
                    return 1;
            }
        } else {

            find_new_solutions(ctx->keys, ctx->min_bit, ctx->max_bit, 
                    ctx->solutions);
        }  
    }

    return 0;
}
 
/* 
 * Called whenever base address is changed. 
 * Tries to find solution.
 * Returns true if solution found. Else return false.
 */
static bool change_base_address_and_find_solutions(hash_context_t *ctx)
{
    if (try_find_all_solutions(ctx)) {
        
        /* Find if any bit possibly not covered */
        for (int i = ctx->min_bit; i <= ctx->max_bit; i++) {
            if ((ctx->global_unexplored_bits >> i) & 0x1)
                fprintf(stderr, "Warning: Bit(%d) possibly not covered in solution\n", i);
        }
        return 1;
    }

    /* Solutions based on keys have been found. Discard the key pair */
    ctx->keys.clear();
   
    /* Reset explored bits */
    ctx->unexplored_bits = (uintptr_t)-1ULL;

    ctx->cur_bit_to_explore = ctx->max_bit;

    ctx->base_addr = rand() % (ctx->end_addr - ctx->start_addr + 1) + ctx->start_addr;
    ctx->base_addr &= ~((1ULL << ctx->min_bit) - 1);
    return 0;
}

/* Find highest bit set in mask <= ceiling bit */
static int find_highest_bit(uintptr_t mask, int ceiling)
{
    int start = std::min(ceiling, MAX_NUM_INDEX - 1);
    for (int i = start; i >= 0; i--) {
        if (mask & (1ULL << i))
            return i;
    }

    return -1;
}

/* Mark all explored bits. Explored bits are bits which differ in two addresses */
static void mark_explored_bits(uintptr_t *unexplored_mask, uintptr_t addr1, uintptr_t addr2)
{
    uintptr_t old_mask = *unexplored_mask;
    uintptr_t new_mask = old_mask & ~(addr1 ^ addr2);
    *unexplored_mask = new_mask;
}

/* Checks if all bits in [min_bit, max_bit] have been set */
static bool are_all_bits_explored(uintptr_t unexplored_mask, int min_bit, int max_bit)
{
    for (int i = min_bit; i <= max_bit; i++) {
        if (unexplored_mask & (1ULL << i))
            return false;
    }

    return true;
}

/* Called when is it confirmed that the pair of address lie in same partition */
static void hash_confirm_pair(hash_context_t *ctx, uintptr_t phy_addr1, uintptr_t phy_addr2)
{
    std::pair <uintptr_t, uintptr_t> key (phy_addr1, phy_addr2);

    ctx->keys.push_back(key);

    mark_explored_bits(&ctx->unexplored_bits, phy_addr1, phy_addr2);
    mark_explored_bits(&ctx->global_unexplored_bits, phy_addr1, phy_addr2);

}

/* Eliminate equivalent solutions to get only unique solutions */
static void hash_reduce(hash_context_t *ctx)
{
    std::vector<solution_t> perm_sarray;
    std::vector<solution_t>::iterator s;

    for (s = ctx->solutions.begin(); s != ctx->solutions.end();) {

        bool is_unique = true;
        
        for (size_t i = 0; i < perm_sarray.size(); i++) {
            if (are_solutions_same(*s, perm_sarray[i])) {
                is_unique = false;
                break;
            }   
        }

        if (is_unique) {
            try {
                add_solution_with_permuations(perm_sarray, *s);
            } catch(...) {
                /* Out of memory - Permutation grows exponentially */
                fprintf(stderr, "Out of memory exception while hash_reduce\n");
                return;
            }
            s++;
        } else {
            s = ctx->solutions.erase(s);
        }
    }
}
/* 
 * Initializes the hash context. 
 * Min bit is the minimum bit to participate in the hash functions.
 * Max bit is the maximum bit to participate in the hash functions.
 * Start and End address are the range of address that can be tested.
 */
hash_context_t *hash_init(int min_bit, int max_bit,
        void *start_addr, void *end_addr)
{
    size_t length;
    hash_context_t *ctx = NULL;

    if (max_bit <= min_bit)
        return NULL;
    
    if (max_bit >= MAX_NUM_INDEX)
        return NULL;

    if (min_bit < 0)
        return NULL;

    if ((uintptr_t)end_addr <= (uintptr_t)start_addr)
        return NULL;

    /* Do we have sufficient address space to find all the bits? */
    length = (uintptr_t)end_addr - (uintptr_t)start_addr;
    if (length < (1ULL << max_bit))
        return NULL;

    ctx = new hash_context_t();
    
    ctx->min_bit = min_bit;
    ctx->max_bit = max_bit;
    
    ctx->start_addr = (uintptr_t)start_addr;
    ctx->end_addr = (uintptr_t)end_addr;

    ctx->global_unexplored_bits = (uintptr_t)-1ULL;

    ctx->is_valid_pair = false;
    
    return ctx;
}

#if 0
/* 
 * Runs till a solution is found. This function might take a long time
 * to execute.
 * Takes a callback function as argument:
 *   Given an address 'addr1', find another address such that it lies on same
 *   partition as 'addr1', lies between 'start_addr' and 'end_addr'.
 *   Test addresses with an  offset of 'offset'. 
 *   Return NULL if no such address found.
 *
 * Returns 0 if solution found.
 * Returns < 0 if no solutions found.
 */
int hash_find_solutions(hash_context_t *ctx, void *arg,
    void *(*find_partition_pair)(void *addr1, void *start_addr, 
        void *end_addr, size_t offset, void *arg))
{
    /* Brute force check all pairs and then brute force to find solutions */
    void *addr;

    ctx->base_addr = ctx->start_addr;
    ctx->test_addr = ctx->start_addr;

    while (ctx->test_addr < ctx->end_addr) {

        addr = find_partition_pair((void *)ctx->base_addr, (void *)ctx->test_addr, 
            (void *)ctx->end_addr, 1ULL << ctx->min_bit, arg);
        if (addr) {
            hash_confirm_pair(ctx, ctx->base_addr, (uintptr_t)addr);
        }

        ctx->test_addr = (uintptr_t)addr + (1ULL << ctx->min_bit);
    }

    if (try_find_all_solutions(ctx))
        return 0;
    
    return -1;
}
#endif

#if 0
/* 
 * Runs till a solution is found. This function might take a long time
 * to execute.
 * Takes a callback function as argument:
 *   Given an address 'addr1', find another address such that it lies on same
 *   partition as 'addr1', lies between 'start_addr' and 'end_addr'.
 *   Test addresses with an  offset of 'offset'. 
 *   Return NULL if no such address found.
 *
 * Returns 0 if solution found.
 * Returns < 0 if no solutions found.
 */
int hash_find_solutions(hash_context_t *ctx, void *arg,
    void *(*find_partition_pair)(void *addr1, void *start_addr, 
        void *end_addr, size_t offset, void *arg))
{
    int new_bit_to_explore;

    if (!ctx->is_valid_pair) {

        ctx->is_valid_pair = true;
        ctx->cur_addr1 = ctx->start_addr;
        ctx->cur_addr2 = ctx->start_addr;
        change_base_address(ctx);
    
    } 
    
    new_bit_to_explore = find_highest_bit(ctx->unexplored_bits, ctx->cur_bit_to_explore);
    if (new_bit_to_explore != ctx->cur_bit_to_explore) {
        ctx->cur_bit_to_explore = new_bit_to_explore;
        ctx->cur_addr2 = ctx->start_addr;
    } else {
        ctx->cur_addr2 += 1ULL << ctx->min_bit;
    }

    while (true) {
        if ((ctx->cur_addr1 & (1ULL << ctx->cur_bit_to_explore)) ==
            (ctx->cur_addr2 & (1ULL << ctx->cur_bit_to_explore))) {
        
            ctx->cur_addr2 += (1ULL << ctx->cur_bit_to_explore);
            ctx->cur_addr2 &= ~((1ULL << ctx->cur_bit_to_explore) - 1);
        }
    
        if (ctx->cur_addr2 <= ctx->end_addr)
            break;

        ctx->cur_bit_to_explore = find_highest_bit(ctx->unexplored_bits, ctx->cur_bit_to_explore - 1);

        if (ctx->cur_bit_to_explore >= ctx->min_bit) {
            ctx->cur_addr2 = ctx->start_addr;
            continue;
        }

        ctx->cur_addr1 += 1ULL << ctx->min_bit;
        change_base_address(ctx);

        if (ctx->cur_addr1 <= ctx->end_addr) {
            ctx->cur_addr2 = ctx->start_addr;
            continue;
        }

        ctx->reached_end = true;
        return -1;
    }

    *phy_addr1 = (void *)ctx->cur_addr1;
    *phy_addr2 = (void *)ctx->cur_addr2;

    return 1;

}
#endif

/*
 * Give a set of solutions, try seeing if a new bit can 
 * be added into the sets of solutions.
 */
static void try_accomodate_new_bit(hash_context_t *ctx, int new_bit, void *arg,
        bool (*check_partition_pair)(void *addr1, void *addr2, void *arg))
{
    std::vector<std::vector<solution_t>> all_new_solutions;
    std::vector<solution_t> new_solutions;
    int ret;
    int num_solutions = ctx->solutions.size();

    assert(num_solutions < 8 * sizeof(uint64_t));

    /* Permute all new solutions */
    for (uint64_t i = 0; i <= (1ULL << num_solutions) - 1; i++) {
        new_solutions = ctx->solutions;
        for (int j = 0; j < 8 * sizeof(uint64_t); j++) {
            if (i & (1ULL << j)) {
                ret = insert_bit_in_solutions(new_solutions[j], new_bit);
                assert(ret >= 0);
            }
        }
        all_new_solutions.push_back(new_solutions);
    }

    /* Try eliminating all but one of the new solutions */
    uintptr_t end_addr = ctx->start_addr + (1ULL << (new_bit + 1));
    end_addr = std::min(ctx->end_addr, end_addr);
    ctx->base_addr = ctx->start_addr;
    
    for (ctx->test_addr = ctx->start_addr + (1ULL << new_bit); 
            ctx->test_addr <= end_addr; 
            ctx->test_addr += 1ULL << ctx->min_bit) {
        
        if (check_partition_pair((void *)ctx->base_addr, (void *)ctx->test_addr, arg)) {
            
            std::vector<std::vector<solution_t>>::iterator s;

            hash_confirm_pair(ctx, ctx->base_addr, ctx->test_addr);
       
            for (s = all_new_solutions.begin(); s != all_new_solutions.end();) {
                
                bool is_correct = true;

                for (int i = 0; i < s->size(); i++) {
                    if (!is_solution_correct(ctx->keys, (*s)[i])) {
                        is_correct = false;
                        break;
                    }
                }

                if (!is_correct) {
                    s = all_new_solutions.erase(s);
                } else {
                    s++;
                }
            } 
        }

        if (all_new_solutions.size() <= 1)
            break;
    }

    if (all_new_solutions.size() == 0) {
        fprintf(stderr, "Something went wrong\n");
        return;
    }

    ctx->solutions = all_new_solutions[0];
}

/* 
 * Runs till a solution is found. Faster than "hash_find_solutions"
 * Takes a callback function as argument:
 *   Given a pair of addresses 'addr1' and 'addr2', checks if they lie on same
 *   partition.
 *
 * Returns 0 if solution found.
 * Returns < 0 if no solutions found.
 */
int hash_find_solutions2(hash_context_t *ctx, void *arg,
    bool (*check_partition_pair)(void *addr1, void *addr2, void *arg))
{
    /* 
     * Works in two steps:
     * 1) Find a base solution - 
     *   This is done via brute force. (XXX: Any faster method)
     *   To make it fast, only consider half of the total bits to consider.
     * 2) Using base solution, find out the role of each leftover bit seperately
     * Using only half of the bits in finding base solution exponentially
     * speeds up the process since each extra bit makes the brute force solution
     * 2x slower whereas second step is O(1) for each leftover bit.
     */
    int end_bit = ((ctx->max_bit + ctx->min_bit) + 1) / 2;
    int highest_bit = find_highest_bit(ctx->start_addr, ctx->max_bit);
    end_bit = std::max(end_bit, highest_bit + 1);

    uintptr_t end_addr = (1ULL << (end_bit + 1)) - 1;
    end_addr = std::min(ctx->end_addr, end_addr);
    std::vector<solution_t> new_solutions;

    /* Step 1 */
    printf("Finding base solutions\n");
    ctx->base_addr = ctx->start_addr;
    ctx->test_addr = ctx->start_addr;

    size_t count;
    size_t max_count = (end_addr - ctx->start_addr) / (1ULL << ctx->min_bit) + 1;
    for (count = 0, ctx->test_addr = ctx->start_addr; 
            ctx->test_addr <= end_addr; 
            count++, ctx->test_addr += 1ULL << ctx->min_bit) {

        if (check_partition_pair((void *)ctx->base_addr, (void *)ctx->test_addr, arg))
            hash_confirm_pair(ctx, ctx->base_addr, ctx->test_addr);

        /* Print progress */
        printf("Done:%.1f%%\r", (float)(count * 100)/(float)(max_count));
    }
    printf("\n");

    if (ctx->keys.size() == 0) {
        fprintf(stderr, "Base solution couldn't be found as no pair found\n");
        return -1;
    }

    if (find_new_solutions(ctx->keys, ctx->min_bit, end_bit, ctx->solutions) == 0) {
        fprintf(stderr, "Base solution couldn't be found\n");
        return -1;
    }

    hash_reduce(ctx);

    /* XXX: Remove this check and try eliminate frivilous solutions */
    if (!are_unique_solutions_found(ctx->solutions.size(), ctx->min_bit, end_bit)) {
        fprintf(stderr, "Too many base solutions\n");
        return -1;
    }

    /* Step 2 */
    count = 0;
    max_count = (ctx->max_bit - end_bit);
    printf("Finding overall solutions\n");
    for (int i = end_bit + 1; i <= ctx->max_bit; count++, i++) {
        try_accomodate_new_bit(ctx, i, arg, check_partition_pair);
        /* Print progress */
        printf("Done:%.1f%%\r", (float)(count * 100)/(float)(max_count));
    }
    printf("\n");

    return 0;
}

void hash_print_solutions(hash_context_t *ctx)
{
    for (size_t i = 0; i < ctx->solutions.size(); i++)
        print_solution(ctx->solutions[i]);
}

void hash_del(hash_context *ctx)
{
    delete ctx;
}
