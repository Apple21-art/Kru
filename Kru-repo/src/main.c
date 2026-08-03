#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>


#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/sema.h"
#include "../include/codegen.h"
#include "../include/ast.h"



static char* read_file(
    const char* path
    )
{
    struct stat st;

    if(stat(path, &st) != 0)
        return NULL;


    if(!S_ISREG(st.st_mode))
    {
        /*
            Refuse directories, FIFOs, devices, sockets, etc. On
            some platforms fseek/ftell report a bogus size (or even
            LONG_MAX) for these, which previously overflowed the
            malloc below into a multi-exabyte request.
        */

        return NULL;
    }


    if(st.st_size < 0)
        return NULL;


    FILE* file =
        fopen(
            path,
            "rb"
            );


    if(!file)
        return NULL;


    size_t size =
        (size_t)st.st_size;


    char* buffer =
        malloc(
            size + 1
            );


    if(!buffer)
    {
        fclose(file);
        return NULL;
    }



    size_t read =
        fread(
            buffer,
            1,
            size,
            file
            );


    if(read != size)
    {
        free(buffer);
        fclose(file);
        return NULL;
    }


    buffer[size] = '\0';



    fclose(file);


    return buffer;
}





int main(
    int argc,
    char** argv
    )
{
    if(argc < 2)
    {
        fprintf(
            stderr,
            "usage: kru0 <file.kru>\n"
            );

        return 1;
    }



    char* source =
        read_file(
            argv[1]
            );



    if(!source)
    {
        fprintf(
            stderr,
            "[kru] failed to read file\n"
            );

        return 1;
    }





    Lexer lexer;


    lexer_init(
        &lexer,
        source
        );





    ASTNode* root =
        parse_program(
            &lexer
            );



    if(!root)
    {
        fprintf(
            stderr,
            "[kru] parsing failed\n"
            );

        free(source);

        return 1;
    }




    if(sema_analyze(root) != 0)
    {
        fprintf(
            stderr,
            "[kru] semantic analysis failed\n"
            );

        ast_free(root);
        free(source);
        return 1;
    }





    const char* output_path =
        (argc >= 3) ? argv[2] : "out.c";


    if(codegen_generate(
            root,
            output_path
            ) != 0)
    {
        fprintf(
            stderr,
            "[kru] code generation failed\n"
            );


        ast_free(root);

        free(source);

        return 1;
    }





    ast_free(root);

    free(source);



    printf(
        "[kru] generated %s\n",
        output_path
        );



    return 0;
}