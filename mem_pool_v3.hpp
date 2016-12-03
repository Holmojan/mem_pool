
#if !defined(_DEBUG)
#	if !defined(NDEBUG)
#		define NDEBUG
#	endif
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <memory.h>
#include <assert.h>
#include <map>
#include <tuple>
#include <sstream>
#include <algorithm>


#if !defined(container_of)
#	define container_of(p,s,m) ((s*)(((char*)p)-offsetof(s,m)))
#endif


template<uint8_t layer_count>
class mem_pool_v3
{
	static_assert(layer_count>=8 && layer_count <= 26, "unsupported max_level value!");
protected:
	class dlink
	{
	public:
		struct node {
			node* p_prev;
			node* p_next;
		};
	protected:
		node dummy;
		uint32_t count;
	public:
		dlink() {
			dummy.p_next = dummy.p_prev = &dummy;
			count = 0;
		}
		~dlink() {
			while (!empty()) {
				node* p_node = begin();
				pop(p_node);
				delete p_node;
			}
		}

		node* begin() { return dummy.p_next; }
		node* end() { return &dummy; }


		void push(node* p_node, node* p_where) {
			node* p_prev = p_where->p_prev;
			node* p_next = p_where;
			p_prev->p_next = p_node;
			p_node->p_prev = p_prev;
			p_next->p_prev = p_node;
			p_node->p_next = p_next;
			count++;
		}
		void pop(node* p_where) {
			assert(!empty()); 
			node* p_prev = p_where->p_prev;
			node* p_next = p_where->p_next;
			assert(p_prev->p_next == p_where && p_next->p_prev == p_where);
			p_prev->p_next = p_next;
			p_next->p_prev = p_prev;
			count--;
		}

		void push_back(node* p_node) { push(p_node, end()); }
		void pop_back() { pop(end()->p_prev); }
		void push_front(node* p_node) { push(p_node, begin()); }
		void pop_front() { pop(begin()); }

		uint32_t size() { return count; }
		bool empty() { return count == 0; }
	};

	typedef typename dlink::node dlink_node;


	struct mem_segment {
		uint32_t layer : 5;
		uint32_t used : 1;
		uint32_t index : 26;

		union {
			dlink_node node;
			uint8_t data[1];
		}header;
	};

	enum {
		MEM_DIRECT_LAYER = 31,
		MEM_BASE_SIZE = sizeof(mem_segment),
		MEM_PAGE_SIZE = MEM_BASE_SIZE << layer_count,
		MEM_MAX_LAYER = layer_count - 1,
		MEM_MAX_SEGMENT_INDEX = (1 << layer_count) - 1
	};


	struct mem_page {
		dlink_node	node;
		uint32_t	alloc_count;
		uint8_t		layer;
		uint8_t		data[MEM_PAGE_SIZE];
	};

	class mem_layer {
	protected:
		uint8_t layer;
		dlink	free_link;
	public:
		mem_layer(uint8_t _layer):layer(_layer) {
		}
		~mem_layer() {
			while (!free_link.empty()){
				free_link.pop_back();
			}
		}

		inline uint32_t get_segment_size() {
			return MEM_BASE_SIZE << layer;
		}

		inline uint32_t get_segment_data_size() {
			return get_segment_size() - offsetof(mem_segment, header.data);
		}

		inline mem_page* segment_to_page(mem_segment* p_segment) {
			uint8_t* p_data = (uint8_t*)p_segment - get_segment_size()*p_segment->index;
			return container_of(p_data, mem_page, data);
		}

		void insert_page(mem_page* p_page) {
			p_page->alloc_count = 0;
			p_page->layer = layer;
			uint32_t segment_size = get_segment_size();
			uint32_t segment_count = MEM_PAGE_SIZE / segment_size;
			assert(MEM_PAGE_SIZE % segment_size == 0);
			for (uint32_t i = 0, j = 0; i < segment_count; i++, j += segment_size) {
				mem_segment* p_segment = (mem_segment*)(p_page->data + j);
				p_segment->layer = layer;
				p_segment->used = 0;
				p_segment->index = i;
				free_link.push_back(&(p_segment->header.node));
			}
		}
		void remove_page(mem_page* p_page) {
			assert(p_page->alloc_count == 0);
			assert(p_page->layer == layer);
			uint32_t segment_size = get_segment_size();
			uint32_t segment_count = MEM_PAGE_SIZE / segment_size;
			assert(MEM_PAGE_SIZE % segment_size == 0);
			for (uint32_t i = 0, j = 0; i < segment_count; i++, j += segment_size) {
				mem_segment* p_segment = (mem_segment*)(p_page->data + j);
				assert(p_segment->used == 0);
				free_link.pop(&(p_segment->header.node));
			}
		}

		void* alloc_segment() {
			dlink_node* p_node=free_link.begin();
			free_link.pop_front();
			/////////////////////////////////////////////////////////////////
			uint32_t segment_size = get_segment_size();
			mem_segment* p_segment = container_of(p_node, mem_segment, header.node);
			mem_page* p_page = segment_to_page(p_segment);
			p_segment->used = 1;
			p_page->alloc_count++;
			return p_segment->header.data;
		}

		void free_segment(void* p_data) {
			uint32_t segment_size = get_segment_size();
			mem_segment* p_segment = container_of(p_data, mem_segment, header.data);
			mem_page* p_page = segment_to_page(p_segment);
			assert(p_segment->layer == layer);
			assert(p_segment->used == 1);
			assert(p_page->alloc_count > 0);
			p_segment->used = 0;
			p_page->alloc_count--;
			/////////////////////////////////////////////////////////////////
			free_link.push_back(&(p_segment->header.node));
		}

		bool empty() {
			return free_link.empty();
		}
	};

	inline uint32_t calc_segment_require_size(uint32_t size) {
		return size + offsetof(mem_segment, header.data);
	}
	
	void* alloc_direct(uint32_t size) {
		uint32_t size2 = calc_segment_require_size(size);
		mem_segment* p_segment = (mem_segment*)malloc(size2);
		if (p_segment == nullptr)
			return nullptr;

		p_segment->layer = MEM_DIRECT_LAYER;
		p_segment->used = 1;
		p_segment->index = 0;
		return p_segment->header.data;
	}
	void free_direct(void* ptr) {
		mem_segment* p_segment = container_of(ptr, mem_segment, header.data);
		free(p_segment);
	}
	void* realloc_direct(void* ptr, uint32_t size) {
		mem_segment* p_segment = container_of(ptr, mem_segment, header.data);
		uint32_t size2 = calc_segment_require_size(size);
		mem_segment* p_segment2 = (mem_segment*)realloc(p_segment, size2);
		return p_segment2->header.data;
	}

	mem_page* alloc_page() {
		if (free_page_link.empty())
			return new mem_page;

		dlink_node* p_node = free_page_link.begin();
		free_page_link.pop_front();
		mem_page* p_page = container_of(p_node, mem_page, node);
		return p_page;
	}

	void free_page(mem_page* p_page) {
		free_page_link.push_back(&(p_page->node));
	}

	void destroy_pages() {
		while (!free_page_link.empty()) {
			dlink_node* p_node = free_page_link.begin();
			free_page_link.pop(p_node);

			mem_page* p_page = container_of(p_node, mem_page, node);
			delete p_page;
		}
	}

	mem_layer*	layers[layer_count];
	dlink		using_page_link;
	dlink		free_page_link;

#if defined(MEM_POOL_DETECTED_MEM_LEAKS)
	std::map<void*, std::tuple<uint32_t, const char*, uint32_t>> record;
#endif

public:
	mem_pool_v3() {
		for (int32_t i = 0; i < layer_count; i++) {
			layers[i] = new mem_layer(i);
		}
	}
	~mem_pool_v3() {
		garbage_collection(true);

#if defined(MEM_POOL_DETECTED_MEM_LEAKS)
		{
			std::stringstream sout;
			for (auto itor = record.begin(); itor != record.end(); itor++) {
				sout << "detected memory leaks! \n"
					<< "address: " << itor->first << ", "
					<< "length: " << std::get<0>(itor->second) << ", "
					<< "alloc at file " << std::get<1>(itor->second) << ": "
					<< "line " << std::get<2>(itor->second) << "\n";
			}
#	if defined(_MSC_VER) && defined(OutputDebugString)
			OutputDebugStringA(sout.str().c_str());
#	else
			printf(sout.str().c_str());
#	endif
		}
#endif
		assert(using_page_link.empty());
		for (int32_t i = 0; i < layer_count; i++) {
			//assert(layers[i]->empty());
			delete layers[i];
		}
	}

	void* alloc_memory(uint32_t size) {
		uint32_t size2 = calc_segment_require_size(size);

		if (size2 > get_max_segment())
			return alloc_direct(size);

		mem_layer** pp_layer = std::lower_bound(layers, layers + layer_count, size2,
			[](mem_layer* p_layer,uint32_t size)->bool {
			return p_layer->get_segment_size() < size;
		});

		if (pp_layer == layers + layer_count)
			return nullptr;

		if ((*pp_layer)->empty()){
			mem_page* p_page = alloc_page();
			if (p_page == nullptr)
				return nullptr;

			using_page_link.push_back(&(p_page->node));
			(*pp_layer)->insert_page(p_page);
		}

		return (*pp_layer)->alloc_segment();
	}
#if defined(MEM_POOL_DETECTED_MEM_LEAKS)
	void* alloc_memory(uint32_t size, const char* file, uint32_t line) {
		void* ptr = alloc_memory(size);
		if (ptr != nullptr) {
			record[ptr] = std::make_tuple(size, file, line);
		}
		return ptr;
	}
#endif
	void free_memory(void* ptr) {
		mem_segment* p_segment = container_of(ptr, mem_segment, header.data);
		uint8_t layer = p_segment->layer;

		if (layer == MEM_DIRECT_LAYER) {
			return free_direct(ptr);
		}
		else {
			layers[layer]->free_segment(ptr);
		}

#if defined(MEM_POOL_DETECTED_MEM_LEAKS)
		record.erase(ptr);
#endif
	}
	void* realloc_memory(void* ptr, uint32_t size) {
		if (ptr == nullptr)
			return alloc_memory(size);

		mem_segment* p_segment = container_of(ptr, mem_segment, header.data);
		uint8_t layer = p_segment->layer;
		if (layer == MEM_DIRECT_LAYER)
			return realloc_direct(ptr, size);

		uint32_t size2 = calc_segment_require_size(size);
		if(layers[layer]->get_segment_size() >= size2)
			return ptr;

		void* ptr2 = alloc_memory(size);
		if (ptr2 == nullptr)
			return nullptr;

		uint32_t len = layers[layer]->get_segment_data_size();
		memcpy(ptr2, ptr, len);
		free_memory(ptr);
		return ptr2;
	}

	void garbage_collection(bool completely = false) {
		for (dlink_node* p_node = using_page_link.begin(); p_node != using_page_link.end();) {
			mem_page* p_page = container_of(p_node, mem_page, node);
			dlink_node* p_next = p_node->p_next;
			if (p_page->alloc_count == 0) {
				using_page_link.pop(p_node);
				uint8_t layer = p_page->layer;
				layers[layer]->remove_page(p_page);
				free_page(p_page);
			}
			p_node = p_next;
		}
		if (completely) {
			destroy_pages();
		}
	}

	inline uint32_t get_page_count() {
		return using_page_link.size();
	}
	inline uint32_t get_page_size() {
		return MEM_PAGE_SIZE;
	}
	inline uint32_t get_min_segment() {
		return layers[0]->get_segment_size();
	}
	inline uint32_t get_max_segment() {
		return layers[MEM_MAX_LAYER]->get_segment_size();
	}

	void output_inform() {
		printf("page size  : %d\n", get_page_size());
		printf("max segment: %d\n", get_max_segment());
		printf("total page : %d\n", get_page_count());
		printf("total apply: %d\n", get_page_count()*get_page_size());
		//printf("total alloc: %d\n", total_alloc);
		//if (page_list.size()) {
		//	printf("effect rate: %.2lf%%\n", total_alloc *100.0 / (page_list.size()*sizeof(mem_page)));
		//}
	}
};


#if defined(MEM_POOL_DETECTED_MEM_LEAKS)
#	define alloc_memory(size) alloc_memory(size, __FILE__, __LINE__)
#endif