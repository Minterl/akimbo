CFLAGS = \
	-Wall -Werror -Wextra -g -std=c99 \
	-Iinclude \
	-Itest \
	-DAK_SAFE \
	-DAK_DEBUG

.PHONY: test
test:
	@make test/core
	@make test/strings

# TODO: make this into a parameterized target

.PHONY: test/core
test/core:
	mkdir -p out && \
	cc -o out/test-core.out \
		$(CFLAGS) \
		-fsanitize=address \
		-fsanitize=bounds \
		test/core.c

.PHONY: test/strings
test/strings:
	mkdir -p out && \
	cc -o out/test-strings.out \
		$(CFLAGS) \
		-fsanitize=address \
		-fsanitize=bounds \
		test/strings.c

.PHONY: test/map
test/map:
	mkdir -p out && \
	cc -o out/test-map.out \
		$(CFLAGS) \
		-fsanitize=address \
		-fsanitize=bounds \
		test/map.c

.PHONY: test/alloc
test/alloc:
	mkdir -p out && \
	cc -o out/test-alloc.out \
		$(CFLAGS) \
		-fsanitize=address \
		-fsanitize=bounds \
		test/alloc.c

.PHONY: examples/mnist
examples/mnist:
	mkdir -p out/examples && \
	cc -o out/examples/mnist.out \
		$(CFLAGS) \
		-Iexamples/include \
		-fsanitize=address \
		-fsanitize=bounds \
		examples/mnist/main.c


.PHONY: tools/mirv
tools/mirv:
	mkdir -p out/tools && \
	cc -o out/tools/mirv.out \
		-Wall -Werror -Wextra -g -std=gnu11 \
		-Iinclude \
		-Itest \
		-DAK_SAFE \
		-DAK_DEBUG \
		-Iexamples/include \
		-fsanitize=address \
		-fsanitize=bounds \
		tools/mirv.c
