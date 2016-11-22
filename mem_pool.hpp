
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <memory.h>
#include <type_traits>
#include <vector>

template<uint8_t max_level>
class mem_pool
{
protected:
	static_assert(max_level>8 && max_level <= 26, "unsupported max_level value!");

	template<uint32_t size>
	struct mem_segment {
		uint32_t	index	:26;
		uint32_t    level	:5;
		uint32_t    direct	:1;
		uint8_t     data[size];
	};
	typedef mem_segment<sizeof(void*)> mem_unit;

	enum {
		N = 1 << max_level,
		M = sizeof(mem_unit),
		K = offsetof(mem_unit, data)
	};

	class mem_page {
	public:
		typedef uint8_t bitmap_type;
		enum {
			BITMAP_TYPE_BITS = sizeof(bitmap_type) / sizeof(uint8_t) * 8,
			BITMAP_SIZE = (N * 2 + BITMAP_TYPE_BITS - 1) / BITMAP_TYPE_BITS,
			BITMAP_MOD_MASK = BITMAP_TYPE_BITS - 1,
			BITMAP_NIL_NODE = ~0
		};

		uint32_t page_index;
		bitmap_type bitmap_and[BITMAP_SIZE];
		bitmap_type bitmap__or[BITMAP_SIZE];
		uint32_t level_count[max_level + 1];
		mem_unit units[N];

		mem_page() {
			page_index = 0;
			static_assert(M*N == sizeof(units), "byte align error!");
			memset(bitmap_and, 0, sizeof(bitmap_and));
			memset(bitmap__or, 0, sizeof(bitmap__or));
			for (uint8_t i = 0; i <= max_level; i++) {
				level_count[i] = (1 << (max_level - i));
			}
		}

		inline static bool get_bit(bitmap_type bitmap[], uint32_t index) {
			return (bitmap[index / BITMAP_TYPE_BITS] >> (index & BITMAP_MOD_MASK)) & 1;
		}
		inline static void set_bit_0(bitmap_type bitmap[], uint32_t index) {
			bitmap[index / BITMAP_TYPE_BITS] &= ~(1 << (index & BITMAP_MOD_MASK));
		}
		inline static void set_bit_1(bitmap_type bitmap[], uint32_t index) {
			bitmap[index / BITMAP_TYPE_BITS] |= (1 << (index & BITMAP_MOD_MASK));
		}

		inline static uint32_t node_to_index(uint32_t p, uint8_t level) {
			return (p << level) - (1 << max_level);
		}

		inline static uint32_t index_to_node(uint32_t i, uint8_t level) {
			return (i + (1 << max_level)) >> level;
		}

		uint32_t lock(uint32_t p, uint8_t cur_level, uint8_t level) {
			if (get_bit(bitmap_and, p))
				return BITMAP_NIL_NODE;

			if (cur_level == level) {
				if (get_bit(bitmap__or, p))
					return BITMAP_NIL_NODE;

				for (uint8_t i = 0; i <= cur_level; i++) {
					level_count[i] -= (1 << (cur_level - i));
				}
				set_bit_1(bitmap__or, p);
				set_bit_1(bitmap_and, p);
				return p;
			}

			if (cur_level>level) {
				uint32_t l = p * 2;
				uint32_t r = l + 1;
				
				uint32_t node = lock(l, cur_level - 1, level);
				if (node!= BITMAP_NIL_NODE) {
					if (!get_bit(bitmap__or, p))
						level_count[cur_level]--;
					set_bit_1(bitmap__or, p);
					if (get_bit(bitmap_and, l) && get_bit(bitmap_and, r))
						set_bit_1(bitmap_and, p);
					return node;
				}
				
				/*uint32_t*/ node = lock(r, cur_level - 1, level);
				if (node != BITMAP_NIL_NODE) {
					set_bit_1(bitmap__or, p);
					if (get_bit(bitmap_and, l) && get_bit(bitmap_and, r))
						set_bit_1(bitmap_and, p);
					return node;
				}
				return BITMAP_NIL_NODE;
			}
			return BITMAP_NIL_NODE;
		}
		void unlock(uint32_t p, uint8_t cur_level, uint8_t level) {
			if (cur_level == level) {
				for (uint8_t i = 0; i <= cur_level; i++) {
					level_count[i] += (1 << (cur_level - i));
				}
				set_bit_0(bitmap__or, p);
				set_bit_0(bitmap_and, p);

				unlock(p / 2, cur_level + 1, level);
				return;
			}
			if (cur_level <= max_level) {
				uint32_t l = p * 2;
				uint32_t r = l + 1;
				if (!(get_bit(bitmap__or, l) || get_bit(bitmap__or, r))) {
					if (get_bit(bitmap__or, p))
						level_count[cur_level]++;
					set_bit_0(bitmap__or, p);
				}
				set_bit_0(bitmap_and, p);

				unlock(p / 2, cur_level + 1, level);
				return;
			}
		}

		bool empty() {
			return level_count[max_level] == 1;
		}

		friend bool operator>(const mem_page& x, const mem_page& y) {
			for (uint8_t i = max_level; i != (uint8_t)-1; i--) {
				if (x.level_count[i] > y.level_count[i]) return true;
				if (x.level_count[i] < y.level_count[i]) return false;
			}
			return false;
		}
	};
protected:
	//std::list<mem_page*> page_list;
	//uint64_t total_alloc;
	std::vector<mem_page*> page_heap;

	inline uint32_t calc_page_max_alloc() {
		return M*N - K;
	}
	inline uint32_t calc_level_max_alloc(uint8_t level) {
		return M*(1 << level) - K;
	}
	inline uint32_t calc_unit_alloc_count(uint32_t size) {
		size += K;
		return (size + M - 1) / M;
	}

	mem_unit* alloc_direct(uint32_t size) {
		size += K;
		mem_unit* punit = (mem_unit*)malloc(size);
		if (punit != nullptr) {
			memset(punit, 0, K);
			punit->direct = 1;
		}
		return punit;
	}
	void free_direct(mem_unit* punit) {
		free(punit);
	}
	mem_unit* realloc_direct(mem_unit* punit, uint32_t size) {
		size += K;
		mem_unit* punit2 = (mem_unit*)realloc(punit, size);
		return punit2;
	}

	mem_unit* alloc_from_page(mem_page* ppage, uint8_t level) {
		uint32_t node = ppage->lock(1, max_level, level);
		if (node == mem_page::BITMAP_NIL_NODE) return nullptr;

		uint32_t index = ppage->node_to_index(node, level);
		mem_unit* punit = &ppage->units[index];
		punit->index = index;
		punit->level = level;
		punit->direct = 0;
		return punit;
	}
	void free_to_page(mem_unit* punit) {
		uint32_t offset = offsetof(mem_page, units[punit->index]);
		mem_page* ppage = (mem_page*)((uint8_t*)punit - offset);
		uint32_t node=ppage->index_to_node(punit->index, punit->level);
		ppage->unlock(node, punit->level, punit->level);

		adjust_heap_from_bottom(ppage->page_index);
	}

	void swap_page(uint32_t x, uint32_t y) {
		std::swap(page_heap[x], page_heap[y]);
		page_heap[x]->page_index = x;
		page_heap[y]->page_index = y;
	}
	void adjust_heap_from_top(uint32_t p) {
		uint32_t l = p * 2;
		if (page_heap.size()>l && *page_heap[l] > *page_heap[p]) {
			swap_page(l, p);
			adjust_heap_from_top(l);
			return;
		}
		uint32_t r = l + 1;
		if (page_heap.size()>r && *page_heap[r] > *page_heap[p]) {
			swap_page(r, p);
			adjust_heap_from_top(r);
			return;
		}
	}

	void adjust_heap_from_bottom(uint32_t c) {
		if (c <= 1) return;
		uint32_t p = c / 2;
		if (*page_heap[c] > *page_heap[p]) {
			swap_page(c,p);
			adjust_heap_from_bottom(p);
		}
	}

	void adjust_heap_for_pop(uint32_t p) {

		uint32_t l = p * 2;
		uint32_t r = l + 1;

		if (page_heap.size() > r && *page_heap[r] > *page_heap[l]) {
			swap_page(r, p);
			adjust_heap_for_pop(r);
		}
		else if (page_heap.size() > l) {
			swap_page(l, p);
			adjust_heap_for_pop(r);
		}
	}

#if !defined(_MSC_VER) || _MSC_VER>1600   
	mem_pool(const mem_pool&) = delete;
	mem_pool(const mem_pool&&) = delete;
	mem_pool& operator=(const mem_pool&) = delete;
	mem_pool& operator=(const mem_pool&&) = delete;
#else
	mem_pool(const mem_pool&);
	mem_pool& operator=(const mem_pool&);

#	if defined(_MSC_VER) && _MSC_VER==1600
	mem_pool(const mem_pool&&);
	mem_pool& operator=(const mem_pool&&);
#	endif

#endif

public:
	mem_pool() {
		page_heap.reserve((1 << 30) / (N*M));
		page_heap.push_back(nullptr);
	}
	virtual ~mem_pool() {
		for (uint32_t i = 0; i < page_heap.size(); i++) {
			mem_page* ppage = page_heap[i];
			delete ppage;
		}
		page_heap.clear();
	}

	void* alloc_memory(uint32_t size) {
		if (size == 0)
			return nullptr;

		if (size>calc_page_max_alloc()) {
			mem_unit* punit = alloc_direct(size);
			if (punit == nullptr) return nullptr;
			return punit->data;
		}

		uint32_t unit_count = calc_unit_alloc_count(size);
		uint8_t level = 0;
		while (((uint32_t)1 << level)<unit_count) level++;

		if (page_heap.size() > 1) {
			mem_page* ppage = page_heap[1];
		
			mem_unit* punit = alloc_from_page(ppage, level);
			if (punit != nullptr) {
				adjust_heap_from_top(1);
				//total_alloc += size;
				return punit->data;
			}
		}

		{
			mem_page* ppage = new mem_page;
			if (ppage == nullptr) return nullptr;
			
			mem_unit* punit = alloc_from_page(ppage, level);
			page_heap.push_back(ppage);
			adjust_heap_from_bottom(page_heap.size()-1);
			if (punit != nullptr) {
				//total_alloc += size;
				return punit->data;
			}
		}
		return nullptr;
	}
	void free_memory(void* ptr) {
		if (ptr == nullptr)
			return;

		mem_unit* punit = (mem_unit*)((uint8_t*)ptr - K);
		if (punit->direct==1)
			return free_direct(punit);

		free_to_page(punit);
		////////////////////

	}
	void* realloc_memory(void* ptr, uint32_t size) {
		if (ptr == nullptr)
			return alloc_memory(size);

		mem_unit* punit = (mem_unit*)((uint8_t*)ptr - K);
		if (punit->direct==1) {
			mem_unit* punit2 = realloc_direct(punit, size);
			if (punit2 == nullptr) return nullptr;
			return punit2->data;
		}

		uint32_t unit_count = calc_unit_alloc_count(size);
		uint8_t level = 0;
		while ((1 << level)<unit_count) level++;

		if (level <= punit->level)
			return ptr;

		mem_unit* ptr2 = alloc_memory(size);
		if (ptr2 == nullptr) return nullptr;
		uint32_t len = (1 << punit->level)*M - K;
		memcpy(ptr2, ptr, len);
		free_memory(ptr);
		return ptr2;
	}
	
	void garbage_collection() {
		while (page_heap.size() > 1 && page_heap[1]->empty()){
			adjust_heap_for_pop(1);
			delete page_heap.back();
			page_heap.pop_back();
		}
	}

	uint32_t get_page_count() {
		return page_heap.size()-1;
	}
	uint32_t get_page_size() {
		return sizeof(mem_page);
	}
	void output_inform() {
		printf("   %d", page_heap.back()->level_count[max_level]);
		//printf("total page : %d\n", page_heap.size());
		//printf("total apply: %d\n", page_heap.size()*sizeof(mem_page));
		//printf("total alloc: %d\n", total_alloc);
		//if (page_list.size()) {
		//	printf("effect rate: %.2lf%%\n", total_alloc *100.0 / (page_list.size()*sizeof(mem_page)));
		//}
	}
};
