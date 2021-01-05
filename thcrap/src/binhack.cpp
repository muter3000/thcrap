/**
  * Touhou Community Reliant Automatic Patcher
  * Main DLL
  *
  * ----
  *
  * Binary hack handling.
  */

#include "thcrap.h"
#include <math.h>
#include <locale.h>
#include <string.h>

/*
 * Grumble, grumble, C is garbage and will only do string→float conversion
 * using the decimal separator from the current locale, and OF COURSE it never
 * occured to anyone, not even Microsoft, to provide a neutral strtod() that
 * always looks for a decimal point, and so we have to dynamically allocate
 * (and free) The Neutral Locale instead. C is garbage.
 */
_locale_t lc_neutral = nullptr;

int hackpoints_error_function_not_found(const char *func_name, int retval)
{
	if (runconfig_msgbox_invalid_func()) {
		if (log_mboxf("Binary Hack error", MB_OKCANCEL | MB_ICONERROR, "ERROR: function '%s' not found! ", func_name) == IDCANCEL) {
			thcrap_ExitProcess(0);
		}
	} else {
		log_printf("ERROR: function '%s' not found! "
#ifdef _DEBUG
			"(implementation not exported or still missing?)"
#else
			"(outdated or corrupt %s installation, maybe?)"
#endif
			"\n", func_name, PROJECT_NAME_SHORT()
		);
	}
	return retval;
}

int is_valid_hex(char c)
{
	return
		('0' <= c && c <= '9') ||
		('A' <= c && c <= 'F') ||
		('a' <= c && c <= 'f');
}

enum value_type_t {
	VT_NONE = 0,
	VT_BYTE = 1, // sizeof(char)
	VT_WORD = 2, // sizeof(short)
	VT_DWORD = 5, // sizeof(dword)
	VT_FLOAT = 4, // sizeof(float)
	VT_DOUBLE = 8 // sizeof(double)
};

struct value_t {
	value_type_t type = VT_NONE;
	union {
		unsigned char b;
		unsigned short w;
		unsigned int i;
		float f;
		double d;
		unsigned char byte_array[8];
	};

	size_t size() {
		return (size_t)(type == VT_DWORD ? VT_FLOAT : type);
	}
};

// Returns false only if parsing should be aborted.
bool consume_value(value_t &val, const char** str)
{
	assert(str);

	const char *c = *str;

	// Double / float
	if(*c == '+' || *c == '-') {
		if(!lc_neutral) {
			lc_neutral = _create_locale(LC_NUMERIC, "C");
		}
		char *endptr;

		errno = 0;
		double result = _strtod_l(*str, &endptr, lc_neutral);
		if(errno == ERANGE && (result == HUGE_VAL || result == -HUGE_VAL)) {
			auto val_len = (endptr - *str);
			log_printf(
				"ERROR: Floating point constant \"%.*s\" out of range!\n",
				val_len, str
			);
			return false;
		} else if(endptr == *str) {
			// Not actually a floating-point number, keep going though
			*str += 1;
			return true;
		}
		if(*endptr == 'f') {
			val.type = VT_FLOAT;
			val.f = (float)result;
			endptr++;
		} else {
			val.type = VT_DOUBLE;
			val.d = result;
		}
		if(*endptr != ' ' && *endptr != '\0') {
			val.type = VT_NONE;
			*str += 1;
		} else {
			*str = endptr;
		}
	}
	// Byte
	else if(is_valid_hex(c[0]) && is_valid_hex(c[1])) {
		char conv[3];
		conv[2] = 0;
		memcpy(conv, *str, 2);
		val.type = VT_BYTE;
		val.b = (unsigned char)strtol(conv, nullptr, 16);
		*str += 2;
	}
	// Nothing, keep going
	else {
		*str += 1;
	}
	return true;
}

size_t binhack_calc_size(const char *binhack_str)
{
	size_t size = 0;
	const char *c = binhack_str;
	if(!c) {
		return 0;
	}
	char current_char;
	while (current_char = *c) {
		switch (current_char) {
			case '(':
			//case '{':
				if (strnicmp(c, "i8:", 3) == 0 || strnicmp(c, "u8:", 3) == 0) {
					c += 3;
					size += 1;
				} else if (strnicmp(c, "i16:", 4) == 0 || strnicmp(c, "u16:", 4) == 0) {
					c += 4;
					size += 2;
				} else if (strnicmp(c, "i32:", 4) == 0 || strnicmp(c, "u32:", 4) == 0 || strnicmp(c, "f32:", 3) == 0) {
					c += 4;
					size += 4;
				} else if (strnicmp(c, "f64:", 4) == 0) {
					c += 4;
					size += 8;
				} else {
					//log_printf("WARNING: no binhack expression size specified, assuming dword...\n");
			case '[':
			case '<':
					size += 4;
				}
				++c;
				c = parse_brackets(c, current_char);
				if (*c == current_char) {
					//Bracket error
					return 0;
				}
				break;
			case '?':
				if (c[1] == '?') {
					// Found a wildcard byte, so just add
					// a byte of size and keep going
					++size;
					c += 2;
					break;
				}
			default: {
				value_t val;
				if (!consume_value(val, &c)) {
					return 0;
				}
				size += val.size();
			}
		}
	}
	return size;
}

bool binhack_from_json(const char *name, json_t *in, binhack_t *out)
{
	if (!json_is_object(in)) {
		log_printf("binhack %s: not an object\n", name);
		return false;
	}

	json_t *addr = json_object_get(in, "addr");
	const char *title = json_object_get_string(in, "title");;
	const char *code = json_object_get_string(in, "code");
	const char *expected = json_object_get_string(in, "expected");
	bool ignore = json_object_get_evaluate_bool(in, "ignore");
	
	if (ignore) {
		log_printf("binhack %s: ignored\n", name);
		return false;
	}
	if (!code || json_flex_array_size(addr) == 0) {
		// Ignore binhacks with missing fields
		// It usually means the binhack doesn't apply for this game or game version.
		return false;
	}

	out->name = strdup(name);
	out->title = strdup(title);
	out->code = strdup(code);
	out->expected = strdup(expected);
	out->addr = new char*[(json_flex_array_size(addr) + 1)];

	size_t i;
	json_t *it;
	json_flex_array_foreach(addr, i, it) {
		if (json_is_string(it)) {
			out->addr[i] = strdup(json_string_value(it));
		}
	}
	out->addr[i] = nullptr;

	return true;
}

int binhack_render(BYTE *binhack_buf, size_t target_addr, const char *binhack_str)
{
	const char *c = binhack_str;
	const char* copy_ptr;
	size_t written = 0;
	char func_name_end = ']';

	if(!binhack_buf || !binhack_str) {
		return -1;
	}
	
	while(c[0]) {
		value_t val;
		switch (c[0]) {
			case '(':
			case '{':
				func_name_end = c[0] == '(' ? ')' : '}';
				++c;
				if (strnicmp(c, "i8:", 3) == 0 || strnicmp(c, "u8:", 3) == 0) {
					c += 3;
					val.type = VT_BYTE;
				} else if (strnicmp(c, "i16:", 4) == 0 || strnicmp(c, "u16:", 4) == 0) {
					c += 4;
					val.type = VT_WORD;
				} else if (strnicmp(c, "i32:", 4) == 0 || strnicmp(c, "u32:", 4) == 0) {
					c += 4;
					val.type = VT_DWORD;
				} else if (strnicmp(c, "f32:", 4) == 0) {
					c += 4;
					val.type = VT_FLOAT;
				} else if (strnicmp(c, "f64:", 4) == 0) {
					c += 4;
					val.type = VT_DOUBLE;
				} else {
					//log_printf("WARNING: no binhack expression size specified, assuming dword...\n");
					val.type = VT_DWORD;
				}
				goto ParseBrackets;
			case '[':
				func_name_end = ']';
				val.type = VT_DWORD;
				goto ParseBrackets;
			case '<':
				func_name_end = '>';
				val.type = VT_DWORD;
ParseBrackets:
				//copy_ptr = eval_expr(c, &val.i, func_name_end, NULL, target_addr + written);
				copy_ptr = eval_expr(c, &val.i, func_name_end, NULL, target_addr + written);
				if (c == copy_ptr) {
					log_printf("Binhack render error!\n");
					return 1;
				}
				c = copy_ptr;
				
				if (val.type == VT_FLOAT) {
					val.f = (float)val.i;
				} else if (val.type == VT_DOUBLE) {
					val.d = (double)val.i;
				}
				break;
			case '?':
				if (c[1] == '?') {
					// Found a wildcard byte, so read the contents
					// of the appropriate address into the buffer.
					//val.b = *(unsigned char*)(target_addr + written);
					val.b = *(unsigned char*)target_addr;
					val.type = VT_BYTE;
					c += 2;
					break;
				}
			default:
				if (!consume_value(val, &c)) {
					return 1;
				}
		}
		switch (val.type) {
			case VT_BYTE:
				*(uint8_t*)binhack_buf = val.b;
				log_printf("Binhack rendered: %hhX at %p\n", *(uint8_t*)binhack_buf, target_addr + written);
				binhack_buf += sizeof(uint8_t);
				written += sizeof(uint8_t);
				break;
			case VT_WORD:
				*(uint16_t*)binhack_buf = val.w;
				log_printf("Binhack rendered: %hX at %p\n", *(uint16_t*)binhack_buf, target_addr + written);
				binhack_buf += sizeof(uint16_t);
				written += sizeof(uint16_t);
				break;
			case VT_DWORD:
				*(uint32_t*)binhack_buf = val.i;
				log_printf("Binhack rendered: %X at %p\n", *(uint32_t*)binhack_buf, target_addr + written);
				binhack_buf += sizeof(uint32_t);
				written += sizeof(uint32_t);
				break;
			case VT_FLOAT:
				*(float*)binhack_buf = val.f;
				log_printf("Binhack rendered: %X at %p\n", *(float*)binhack_buf, target_addr + written);
				binhack_buf += sizeof(float);
				written += sizeof(float);
				break;
			case VT_DOUBLE:
				*(double*)binhack_buf = val.d;
				log_printf("Binhack rendered: %llX at %p\n", *(uint64_t*)binhack_buf, target_addr + written);
				binhack_buf += sizeof(double);
				written += sizeof(double);
				break;
		}
		//switch (val.type) {
		//	case VT_BYTE:   copy_ptr = (const char*)&val.b; break;
		//	case VT_WORD:   copy_ptr = (const char*)&val.w; break;
		//	case VT_DWORD:  copy_ptr = (const char*)&val.i; break;
		//	case VT_FLOAT:  copy_ptr = (const char*)&val.f; break;
		//	case VT_DOUBLE: copy_ptr = (const char*)&val.d; break;
		//	default:        break; // -Wswitch...
		//}
		/*binhack_buf = (BYTE *)memcpy_advance_dst(
			(char *)binhack_buf, val.byte_array, val.size()
		);*/
		
		//written += val.size();
	}
	return 0;
}

// Returns the number of all individual instances of binary hacks in [binhacks].
static size_t binhacks_total_count(const binhack_t *binhacks, size_t binhacks_count)
{
	int ret = 0;
	for (size_t i = 0; i < binhacks_count; i++) {
		for (size_t j = 0; binhacks[i].addr[j]; j++) {
			ret++;
		}
	}
	return ret;
}

int binhacks_apply(const binhack_t *binhacks, size_t binhacks_count, HMODULE hMod)
{
	size_t c = 0;
	size_t binhacks_total = binhacks_total_count(binhacks, binhacks_count);
	int failed = binhacks_total;

	if(!binhacks_count) {
		log_printf("No binary hacks to apply.\n");
		return 0;
	}

	log_printf("Applying binary hacks...\n");
	log_printf("------------------------\n");

	for(size_t i = 0; i < binhacks_count; i++) {
		const binhack_t& cur = binhacks[i];

		// calculated byte size of the hack
		size_t asm_size = binhack_calc_size(cur.code);
		size_t exp_size = binhack_calc_size(cur.expected);
		if(!asm_size) {
			continue;
		}

		VLA(BYTE, asm_buf, asm_size);
		VLA(BYTE, exp_buf, exp_size);
		for(size_t j = 0; cur.addr[j]; j++) {
			auto addr = str_address_value(cur.addr[j], hMod, NULL);
			if(!addr) {
				continue;
			}
			log_printf("(%2d/%2d) 0x%p ", ++c, binhacks_total, addr);
			if(cur.title) {
				log_printf("%s (%s)... ", cur.title, cur.name);
			} else {
				log_printf("%s...", cur.name);
			}
			if(binhack_render(asm_buf, addr, cur.code)) {
				continue;
			}
			if(exp_size > 0 && exp_size != asm_size) {
				log_printf("different sizes for expected and new code, skipping verification... ");
				exp_size = 0;
			} else if(binhack_render(exp_buf, addr, cur.expected)) {
				exp_size = 0;
			}
			if(PatchRegion((void*)addr, exp_size ? exp_buf : NULL, asm_buf, asm_size)) {
				log_printf("OK\n");
				failed--;
			} else {
				log_printf("expected bytes not matched, skipping...\n");
			}
		}
		VLA_FREE(asm_buf);
		VLA_FREE(exp_buf);
	}
	log_printf("------------------------\n");
	return failed;
}

bool codecave_from_json(const char *name, json_t *in, codecave_t *out) {
	if (!json_is_object(in)) {
		log_printf("codecave %s: not an object\n", name);
		return false;
	}

	bool ignore = json_object_get_evaluate_bool(in, "ignore");

	if (ignore) {
		log_printf("codecave %s: ignored\n", name);
		return false;
	}

	const char *access = json_object_get_string(in, "access");
	CodecaveAccessType access_val = EXECUTE_READWRITE;

	if (access) {
		BYTE temp = !!strpbrk(access, "rR");
		temp += !!strpbrk(access, "wW") << 1;
		temp += !!strpbrk(access, "eE") << 2;
		switch (temp) {
			case 0:
				log_printf("codecave %s: why would you set a codecave to no access? skipping instead...\n", name);
				return false;
			case 1:
				access_val = (CodecaveAccessType)(temp - 1);
				break;
			case 2:
				log_printf("codecave %s: write-only codecaves can't be set, using read-write instead...\n", name);
			case 3: case 4: case 5:
				access_val = (CodecaveAccessType)(temp - 2);
				break;
			case 6:
				log_printf("codecave %s: execute-write codecaves can't be set, using execute-read-write instead...\n", name);
			case 7:
				access_val = (CodecaveAccessType)(temp - 3);
				break;
		}
	}

	json_t *j_temp = json_object_get(in, "size");
	size_t size_val = 0;

	if (j_temp) {
		if (!json_is_integer(j_temp) && !json_is_string(j_temp)) {
			log_printf("ERROR: invalid value specified for size of codecave %s\n", name);
			return false;
		}
		size_val = json_evaluate_int(j_temp);
		if (!size_val) {
			log_printf("codecave %s with size 0 ignored\n", name);
			return false;
		}
	}

	j_temp = json_object_get(in, "fill");
	BYTE fill_val = 0;

	if (j_temp) {
		if (!json_is_integer(j_temp) && !json_is_string(j_temp)) {
			log_printf("ERROR: invalid value specified for fill value of codecave %s\n", name);
			return false;
		}
		fill_val = (BYTE)json_evaluate_int(j_temp);
	}

	j_temp = json_object_get(in, "count");
	size_t count_val = 1;

	if (j_temp) {
		if (!json_is_integer(j_temp) && !json_is_string(j_temp)) {
			log_printf("ERROR: invalid value specified for count of codecave %s\n", name);
			return false;
		}
		count_val = json_evaluate_int(j_temp);
		if (!count_val) {
			log_printf("codecave %s with count 0 ignored\n", name);
			return false;
		}
	}

	const char *code = json_object_get_string(in, "code");

	out->code = code ? strdup(code) : NULL;
	out->name = strdup(name);
	out->access_type = access_val;
	out->size = size_val;
	out->count = count_val;
	out->fill = fill_val;

	return true;
}

int codecaves_apply(const codecave_t *codecaves, size_t codecaves_count) {
	if (codecaves_count == 0) {
		return 0;
	}

	log_printf("Applying codecaves...\n");
	log_printf("------------------------\n");

	const size_t codecave_sep_size_min = 3;

	//One size for each valid access flag combo
	size_t codecaves_total_size[5] = { 0, 0, 0, 0, 0 };

	struct codecave_local_state_t {
		size_t size;
		size_t size_full;
		bool skip;
	};

	VLA(codecave_local_state_t, codecaves_local_state, (codecaves_count + 1) * sizeof(codecave_local_state_t));
	codecaves_local_state[codecaves_count] = {};

	// First pass: calc the complete codecave size
	for (size_t i = 0; i < codecaves_count; i++) {
		const char* code = codecaves[i].code;
		size_t size = codecaves[i].size;
		if (!code && !size) {
			codecaves_local_state[i].skip = true;
			continue;
		}
		size *= codecaves[i].count;
		size_t calc_size = code ? binhack_calc_size(code) : 0;
		if (calc_size > size) {
			size = calc_size;
		}
		codecaves_local_state[i].size = size;
		if (!size) {
			codecaves_local_state[i].skip = true;
			continue;
		}
		codecaves_local_state[i].skip = false;

		size += codecave_sep_size_min;
		codecaves_local_state[i].size_full = size - (size % 16) + 16;
		codecaves_total_size[codecaves[i].access_type] += codecaves_local_state[i].size_full;
	}

	BYTE* codecave_buf[5];
	for (int i = 0; i < 5; ++i) {
		if (codecaves_total_size[i]) {
			codecave_buf[i] = (BYTE*)VirtualAlloc(NULL, codecaves_total_size[i], MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (!codecave_buf[i]) {
				//Should probably put an abort error here
			}
		} else {
			//Don't try to reallocate a non-existing page later
			codecave_buf[i] = NULL;
		}
	}

	// Second pass: Gather the addresses, so that codecaves can refer to each other.
	BYTE* current_cave[5];
	for (int i = 0; i < 5; ++i) {
		current_cave[i] = codecave_buf[i];
	}

	for (size_t i = 0; i < codecaves_count; i++) {
		if (!codecaves_local_state[i].skip) {
			const char *codecave_name = codecaves[i].name;
			const CodecaveAccessType access = codecaves[i].access_type;

			VLA(char, codecave_full_name, strlen(codecave_name) + 10); // strlen("codecave:") = 9
			strcpy(codecave_full_name, "codecave:");
			strcpy(codecave_full_name + 9, codecave_name);
			func_add(codecave_full_name, (size_t)current_cave[access]);

			current_cave[access] += codecaves_local_state[i].size_full;

			VLA_FREE(codecave_full_name);
		}
	}

	// Third pass: Write all of the code
	for (int i = 0; i < 5; ++i) {
		current_cave[i] = codecave_buf[i];
	}

	for (size_t i = 0; i < codecaves_count; i++) {
		if (!codecaves_local_state[i].skip) {
			const char* code = codecaves[i].code;
			const CodecaveAccessType access = codecaves[i].access_type;
			memset(current_cave[access], codecaves[i].fill, codecaves_local_state[i].size);
			if (code) {
				binhack_render(current_cave[access], (size_t)current_cave[access], code);
			}

			current_cave[access] += codecaves_local_state[i].size;
			for (size_t j = 0; j < codecaves_local_state[i].size_full - codecaves_local_state[i].size; j++) {
				*current_cave[access] = 0xCC; current_cave[access]++;
			}
		}
	}

	VLA_FREE(codecaves_local_state);

	const DWORD page_access_type_array[5] = { PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE };
	DWORD idgaf;

	for (int i = 0; i < 5; ++i) {
		if (codecave_buf[i]) {
			VirtualProtect(codecave_buf[i], codecaves_total_size[i], page_access_type_array[i], &idgaf);
		}
	}
	return 0;
}

extern "C" __declspec(dllexport) void binhack_mod_exit()
{
	SAFE_CLEANUP(_free_locale, lc_neutral);
}
