# Use PWD so Docker volume mounts work reliably across environments
PWD=$(shell pwd)
IMAGE=wfrecorderlite-dev

.PHONY: build binary build-local run clean

# ==========================================
# ENVIRONMENT BUILD
# ==========================================
build:
	@echo "Building the toolchain image..."
	docker build -t $(IMAGE) .

# ==========================================
# PRODUCTION (Runs via Ephemeral Docker)
# ==========================================
binary: build
	@echo "Compiling the binary inside Docker..."
	docker run --rm -v $(PWD):/app $(IMAGE) bash -c "\
		git config --global --add safe.directory /app; \
		rm -rf build-docker; \
		cmake -B build-docker -G Ninja -DCMAKE_BUILD_TYPE=Release && \
		cmake --build build-docker && \
		mkdir -p dist && cp build-docker/wf-recorder-lite dist/wf-recorder-lite"
	@ls -la $(PWD)/dist/wf-recorder-lite

# ==========================================
# LOCAL (on a box with Qt6 + cmake + ninja)
# ==========================================
build-local:
	cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build

run:
	./dist/wf-recorder-lite

clean:
	rm -rf build build-docker
