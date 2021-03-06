include Makefile.in
SUBDIRS = test src common
SUBOBJS = $(addprefix bin/, $(addsuffix .a, $(SUBDIRS)))


all: bin/test bin/db-main bin/m3-database.a #$(SUBDIRS)
	- mkdir -p bin/
	make check

bin/db-main: main.cc ${SUBOBJS}
	${CXX} $^ -o $@ ${CPPFLAGS}

bin/test: test.cc ${SUBOBJS}
	${CXX} $^ -o $@ ${CPPFLAGS}

m3-database.a: bin/m3-database.a

bin/m3-database.a: ${SUBOBJS}
	cd bin && for i in *.a; do ar -x $$i; done
	- ${AR} -r $@ bin/*.o
	- ${RANLIB} $@

$(SUBDIRS): 
	- mkdir -p bin/
	make -C $@ -j4

$(SUBOBJS): $(SUBDIRS)

clean:
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -rf bin/*
	cd e2etest && ls | grep -v "db.tar.gz" | grep -v "prepare.sh" | xargs rm -rf

check: bin/test
	- mkdir -p bin/
	- cd e2etest && ./prepare.sh
	- bin/test

lines: 
	- find . -name \*.hh -print -o -name \*.cc -print | xargs wc -l

.PHONY: clean $(SUBDIRS) test lines

