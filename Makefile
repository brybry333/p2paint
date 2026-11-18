# p2paint — Linux x64 build of the client mod (game_shader_generic_p2paint.so: loaded as a shader library, no shaders)
# Mirrors the Source SDK 2013 (2025) posix VPC settings: see third_party/.../vpc_scripts/source_posix_*.vpc
SDK      := third_party/source-sdk-2013/src
SDS      := $(SDK)/materialsystem/stdshaders
LIBPUB   := $(SDK)/lib/public/linux64
BUILD    := build
OUTNAME  := game_shader_generic_p2paint
OUT      := $(BUILD)/$(OUTNAME).so

CXX      := g++
CC       := gcc
DEFINES  := -DGNUC -DPOSIX -D_POSIX -DCOMPILER_GCC -D_DLL_EXT=.so -D_LINUX -DLINUX -DPLATFORM_64BITS \
            -DVPC -DSOURCESDK -DNDEBUG -DVPROF_LEVEL=1 -DNO_HOOK_MALLOC -D_FILE_OFFSET_BITS=64 \
            -Usprintf -Ustrncpy -UPROTECTED_THINGS_ENABLE
WARN     := -Wall -Wno-multichar -Wno-write-strings -Wno-unused-variable -Wno-unused-function \
            -Wno-sign-compare -Wno-nonnull-compare -Wno-ignored-attributes -Wno-misleading-indentation \
            -Wno-unused-but-set-variable -Wno-unused-local-typedefs -Wno-switch -Wno-parentheses \
            -Wno-unknown-pragmas -Wno-unused-parameter -Wno-unused-value -Wno-comment -Wno-float-equal \
            -Werror=return-type
WARNCXX  := -Wno-overloaded-virtual -Wno-placement-new -Wno-c++20-compat -Wno-mismatched-new-delete \
            -Wno-class-memaccess -Wno-conversion-null -Wno-invalid-offsetof -Wno-reorder \
            -Wno-template-body -Wno-changes-meaning -Wno-deprecated-declarations
OPT      := -O2 -g -fno-strict-aliasing -ffast-math -fno-omit-frame-pointer -fno-delete-null-pointer-checks
COMMON   := -march=x86-64 -fPIC -pipe -fvisibility=hidden $(OPT) $(DEFINES) \
            -I$(SDK)/common -I$(SDK)/public -I$(SDK)/public/tier0 -I$(SDK)/public/tier1
CFLAGS   := $(COMMON) $(WARN)
CXXFLAGS := $(COMMON) $(WARN) $(WARNCXX) -std=gnu++14

# ---- tier1 / mathlib static libs (the SDK ships shaderlib.a + tier2.a prebuilt, not these) -------------
TIER1_SRC := bitbuf newbitbuf byteswap characterset checksum_crc checksum_md5 checksum_sha1 commandbuffer \
             convar datamanager diff exprevaluator generichash ilocalize interface KeyValues keyvaluesjson \
             kvpacker lzmaDecoder lzss mempool memstack NetAdr splitstring processor_detect_linux qsort_s \
             rangecheckedvar reliabletimer stringpool strtools strtools_unicode tier1 tokenreader \
             sparsematrix uniqueid utlbuffer utlbufferutil utlstring utlsymbol utlbinaryblock pathmatch \
             snappy snappy-sinksource snappy-stubs-internal
MATH_SRC  := color_conversion halton lightdesc mathlib_base powsse sparse_convolution_noise sseconst sse 3dnow \
             ssenoise anorms bumpvects IceKey imagequant polyhedron quantize randsse spherical \
             simdvectormatrix vector vmatrix almostequal
TIER1_OBJ := $(TIER1_SRC:%=$(BUILD)/tier1/%.o) $(BUILD)/tier1/LzmaDec.o
MATH_OBJ  := $(MATH_SRC:%=$(BUILD)/mathlib/%.o)

$(BUILD)/tier1/%.o: $(SDK)/tier1/%.cpp
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -DTIER1_STATIC_LIB -c $< -o $@
$(BUILD)/tier1/LzmaDec.o: $(SDK)/utils/lzma/C/LzmaDec.c
	@mkdir -p $(@D); $(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/mathlib/%.o: $(SDK)/mathlib/%.cpp
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -DMATHLIB_LIB -I$(SDK)/public/mathlib -c $< -o $@
$(BUILD)/tier1.a: $(TIER1_OBJ)
	@rm -f $@; ar rcs $@ $^
$(BUILD)/mathlib.a: $(MATH_OBJ)
	@rm -f $@; ar rcs $@ $^

# ---- the shader library --------------------------------------------------------------------------------
SHADER_DEFS := -DSTDSHADER_DX9_DLL_EXPORT -DFAST_MATERIALVAR_ACCESS -DGAME_SHADER_DLL -DDLLNAME=$(OUTNAME)
DLL_SRC  := $(wildcard src/*.cpp)
DLL_OBJ  := $(DLL_SRC:src/%.cpp=$(BUILD)/dll/%.o) $(BUILD)/dll/memoverride.o
DLL_INC  := -I$(SDS) -Isrc

$(BUILD)/dll/%.o: src/%.cpp src/*.h
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) $(SHADER_DEFS) $(DLL_INC) -c $< -o $@
$(BUILD)/dll/memoverride.o: $(SDK)/public/tier0/memoverride.cpp
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) $(SHADER_DEFS) -c $< -o $@

$(OUT): $(DLL_OBJ) $(BUILD)/tier1.a $(BUILD)/mathlib.a
	$(CXX) -shared -o $@ $(DLL_OBJ) -static-libgcc -Wl,--build-id -Wl,-z,noexecstack \
	    -Wl,--start-group $(LIBPUB)/shaderlib.a $(LIBPUB)/tier2.a $(BUILD)/mathlib.a $(BUILD)/tier1.a -Wl,--end-group \
	    -L$(LIBPUB) -ltier0 -lvstdlib -lm -ldl -lpthread -Wl,--no-undefined \
	    -Wl,--version-script=$(SDK)/devtools/version_script.linux.txt

# ---- the LD_PRELOAD loader (CS:S refuses mod shader libraries; see src/loader/p2paint_loader.c) -----
LOADER   := $(BUILD)/libp2paint.so
$(LOADER): src/loader/p2paint_loader.c
	@mkdir -p $(@D); $(CC) -O2 -g -Wall -fPIC -fvisibility=hidden -shared -o $@ $< -ldl

# ---- offline test of the paint-on-surfaces code: build/surftest <game root> <map> [splats.txt] ---------
SURFTEST := $(BUILD)/surftest
$(SURFTEST): tools/surftest.cpp $(BUILD)/dll/p2paint_bsp.o $(BUILD)/dll/p2paint_props.o $(BUILD)/dll/p2paint_surfaces.o \
             $(BUILD)/mathlib.a $(BUILD)/tier1.a
	$(CXX) $(CXXFLAGS) -Isrc $^ -Wl,--start-group $(LIBPUB)/tier2.a $(BUILD)/mathlib.a $(BUILD)/tier1.a -Wl,--end-group \
	    -L$(LIBPUB) -ltier0 -lvstdlib -lm -ldl -lpthread -o $@
surftest: $(SURFTEST)

# ---- the in-memory engine patches on a file: build/patchtest <engine.so> <out> (prints what it would patch) --
PATCHTEST := $(BUILD)/patchtest
$(PATCHTEST): tools/patchtest.cpp src/p2paint_enginepatch.cpp src/p2paint_enginepatch.h
	@mkdir -p $(@D); $(CXX) -O2 -g -Wall -Isrc tools/patchtest.cpp src/p2paint_enginepatch.cpp -o $@
patchtest: $(PATCHTEST)

all: $(OUT) $(LOADER)
clean:
	rm -rf $(BUILD)
.PHONY: all clean surftest patchtest
.DEFAULT_GOAL := all
