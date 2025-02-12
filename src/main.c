#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

#include <libgen.h>
#include <dirent.h>
#include <string.h>

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <errno.h>

#include "./ast.h"
#include "./converter.h"
#include "./lexer.h"
#include "./parser.h"
#include "./strings.h"
#include "./symbols.h"
#include "./tokens.h"
#include "./tst.h"
#include "./codegen.h"
#include "./typer.h"
#include "./opscan.h"
#include "serene.h"

struct Module {
    char* contents;
    struct Tokenvec tokens;
    struct Opdecls ops;
};

struct ModuleNode {
    struct ModuleNodesLL* children;
    char* name;
    struct Module self;
};

struct ModuleNodesLL {
    struct ModuleNodesLL *next;
    struct ModuleNode current;
};

void ModuleNode_print(struct ModuleNode* this, int level) {
    for (int i = 0; i < level; i++) printf("  ");
    printf("%s (%p)\n", this->name, this->self.contents);
    for (struct ModuleNodesLL* head = this->children; head; head = head->next) {
        ModuleNode_print(&head->current, level + 1);
    }
}

struct ModuleNode populate(
    struct serene_Allocator alloc,
    struct serene_Allocator scratch,
    char *dir_name, char *dir_path, int dir_pathlen
) {
    struct ModuleNode out = {0};
    out.name = dir_name;
    DIR *dir = opendir(dir_path);
    for (
        struct dirent* entry = readdir(dir);
        entry;
        entry = readdir(dir)
    ) {
        if (entry->d_name[0] == '.') continue;
        int namelen = strlen(entry->d_name);
        char* full_path = serene_nalloc(scratch, namelen + dir_pathlen + 2, char);
        assert(full_path && "OOM");
        snprintf(full_path, namelen + dir_pathlen + 2, "%s/%s", dir_path, entry->d_name);

        int fd = open(full_path, O_RDWR);
        if (fd != -1) {
            if (strncmp(entry->d_name + namelen - 5, ".tara", 5) != 0) {
                close(fd);
                continue;
            }
            struct stat stat = {0};
            assert(fstat(fd, &stat) == 0);
            struct ModuleNode child = {0};
            child.name = serene_nalloc(alloc, namelen - 4, char);
            snprintf(child.name, namelen - 4, "%s", entry->d_name);
            child.self.contents = mmap(
                NULL,
                stat.st_size,
                PROT_READ,
                MAP_PRIVATE,
                fd,
                0
            );

            struct ModuleNodesLL* tmp = serene_alloc(alloc, struct ModuleNodesLL);
            assert(tmp && "OOM");
            tmp->current = child;
            tmp->next = out.children;
            out.children = tmp;
        } else {
            struct ModuleNode child = populate(
                alloc,
                scratch,
                entry->d_name,
                full_path,
                namelen + dir_pathlen + 1
            );
            struct ModuleNodesLL* head;
            for (head = out.children; head; head = head->next) {
                if (strcmp(head->current.name, child.name) == 0) {
                    assert(head->current.children == NULL);
                    head->current.children = child.children;
                    break;
                }
            }
            if (head == NULL) {
                struct ModuleNodesLL* tmp = serene_alloc(alloc, struct ModuleNodesLL);
                assert(tmp && "OOM");
                tmp->current = child;
                tmp->next = out.children;
                out.children = tmp;
            }
        }
    }
    return out;
}

int main(int argc, char **argv) {
    struct serene_Arena module_arena, strings_arena, opscan_arena, ast_arena, type_arena, check_arena,
        tst_arena, codegen_arena;
    module_arena = strings_arena = opscan_arena = ast_arena = type_arena = check_arena = tst_arena =
        codegen_arena = (struct serene_Arena){
            .backing = serene_Libc_dyn(),
        };

    if (argc != 2) {
        printf("Please provide a single filename!\n");
        return 1;
    }
    char* dir_path = dirname(argv[1]);
    int dir_len = strlen(dir_path);
    char* dir_name = dir_path;
    for (int i = dir_len - 1; i >= 0; i--) {
        if (dir_path[i] != '/') continue;
        dir_name = &dir_path[i + 1];
        break;
    }
    struct ModuleNode modules = populate(
        serene_Arena_dyn(&module_arena),
        serene_Arena_dyn(&opscan_arena),
        dir_name,
        dir_path,
        dir_len
    );
    serene_Arena_deinit(&opscan_arena);
    opscan_arena = (struct serene_Arena){
        .backing = serene_Libc_dyn(),
        .bump = NULL,
        .segments = NULL,
    };

    ModuleNode_print(&modules, 0);

    assert(false && "TODO");

    struct Intern intern = {0};
    intern.alloc = serene_Arena_dyn(&strings_arena);

    struct Symbols symbols = populate_interner(&intern);

    printf("[\n");
    {
        struct Lexer lexer = {
            .rest = file,
            .token = {0},
        };
        for (
            struct LexResult t = Lexer_next(&lexer);
            t.token.kind != TK_EOF;
            t = Lexer_next(&lexer)
        ) {
            printf(
                "\t(%p)\t'%.*s'\n",
                t.token.spelling,
                (int) t.len,
                t.token.spelling
            );
        }
    }
    printf("]\n");

    struct Opdecls ops = {0};
    struct Tokenvec tokenvec = {0};

    {
        struct Lexer lexer = {
            .rest = file,
            .token = {0},
        };
        scan(
            serene_Arena_dyn(&opscan_arena),
            &intern,
            lexer,
            &tokenvec,
            &ops
        );
    }

    struct Tokenstream stream = {
        .buf = tokenvec.buf,
        .len = tokenvec.len,
    };

    printf("[\n");
    {
        struct Tokenstream printer = stream;
        for (
            struct Token t = Tokenstream_peek(&printer);
            t.kind != TK_EOF;
            Tokenstream_drop(&printer), t = Tokenstream_peek(&printer)
        ) {
            printf(
                "\t(%p)\t'%s'\n",
                t.spelling,
                t.spelling
            );
        }
    }
    printf("]\n");

    struct TypeIntern types =
        TypeIntern_init(serene_Arena_dyn(&type_arena), symbols);

    struct Ast ast = parse(
        serene_Arena_dyn(&ast_arena),
        ops,
        &types,
        stream
    );

    munmap(file, filestat.st_size);
    close(filedesc);

    Ast_print(&ast);
    
    typecheck(serene_Arena_dyn(&check_arena), &types, &ast);
    serene_Arena_deinit(&check_arena);

    Ast_print(&ast);

    struct Tst tst = convert_ast(serene_Arena_dyn(&tst_arena), &types, ast);
    LLVMModuleRef mod = lower(&tst, serene_Arena_dyn(&codegen_arena));
    serene_Arena_deinit(&codegen_arena);
    serene_Arena_deinit(&tst_arena);

    printf("----module start----\n");
    LLVMDumpModule(mod);
    printf("----module end----\n");

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    char *emitfile = "out.o";
    const char *triple = LLVMGetDefaultTargetTriple();
    const char *cpu = LLVMGetHostCPUName();
    const char *features = LLVMGetHostCPUFeatures();

    char *error = NULL;
    LLVMTargetRef target;
    if (LLVMGetTargetFromTriple(triple, &target, &error)) {
        printf("error occured!\n%s\n", error);
        assert(false);
    }
    LLVMDisposeMessage(error);
    
    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
        target, triple, cpu, features, LLVMCodeGenLevelNone,
        LLVMRelocDefault, LLVMCodeModelDefault
    );

    error = NULL;
    if (LLVMTargetMachineEmitToFile(
        machine, mod, emitfile, LLVMObjectFile, &error
    )) {
        printf("error occured!\n%s\n", error);
        assert(false);
    }
    LLVMDisposeMessage(error);

    LLVMDisposeTargetMachine(machine);

    LLVMDisposeModule(mod);

    system("ld.lld -o out out.o");

    serene_Arena_deinit(&ast_arena);
    serene_Arena_deinit(&type_arena);
    serene_Arena_deinit(&strings_arena);
    printf("\n");
    return 0;
}

