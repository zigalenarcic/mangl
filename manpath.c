/*
 * manpath.c
 *
 * retrieving paths for man pages from `manpath` executable
 */

/**
 * Buffer size might seem excessive, but keeps the algorithm simple for the
 * moment. Though it's checked, if the buffer was insufficient and a warning
 * is given in case.
 *
 * Using Nix package manager and `nix shell` 500 characters output is reached
 * easily.
 */
#define MAN_PATHS_BUFFER_SIZE 4096

// just for easy debugging
//#define DEBUG_MANPATH(...) fprintf(stderr, __VA_ARGS__)
#define DEBUG_MANPATH(...) while(0)

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int read_stdout_from_manpath(char *buffer)
{
    DEBUG_MANPATH("execute `manpath` and read stdout into string\n");

    FILE *manpath_stdout;
    char *retval;

    manpath_stdout = popen("/usr/bin/env manpath --quiet", "r");
    retval = fgets(buffer, MAN_PATHS_BUFFER_SIZE, manpath_stdout);
    if(retval == NULL)
        return 0;

    DEBUG_MANPATH("checking if buffer was exceeded\n");
    // let's assume there is no filename with a newline
    // `manpath` always ends on a newline
    if(strchr(buffer, '\n') == NULL)
    {
        fprintf(stderr, "Output of `manpath` exceeded buffer of %d.\n", MAN_PATHS_BUFFER_SIZE);
        fprintf(stderr, "Not all paths to man pages will be searched.\n");
    }
    pclose(manpath_stdout);

    DEBUG_MANPATH("%s", buffer);
    return 1;
}
static int allocating_array_for_paths(char *buffer, char const ***manpath_paths)
{
    DEBUG_MANPATH("allocating array for pointers to path\n");

    // XXX: saw too late, that there is stretchy_buffer …
    char *current_path = buffer;
    size_t number_of_manpath_paths = 1;

    while((current_path = strchr(++current_path, ':')) != NULL)
        ++number_of_manpath_paths;
    *manpath_paths = calloc(number_of_manpath_paths, sizeof(char *));
    if(*manpath_paths == NULL)
    {
        fprintf(stderr, "Failed to calloc() memory in `%s` at line %d\n", __FILE__, __LINE__);
        fprintf(stderr, "Falling back original hard coded paths\n");
        return 0;
    }

    DEBUG_MANPATH("allocated space, number of paths: %ld\n", number_of_manpath_paths);
    return number_of_manpath_paths;
}

static void split_buffer_into_paths(char *buffer, char const ***manpath_paths)
{
    DEBUG_MANPATH("splitting buffer into paths\n");

    size_t manpath_paths_index = 0;
    char *current_path, *next_path;
    current_path = strtok_r(buffer, ":\n", &next_path);

    while (current_path != NULL)
    {
        DEBUG_MANPATH("%ld — \"%s\"\n", (current_path - buffer), current_path);
        (*manpath_paths)[manpath_paths_index++] = current_path;
        current_path = strtok_r(NULL, ":\n", &next_path);
    }

    DEBUG_MANPATH("splitting done, paths index: %ld\n", manpath_paths_index);
}

/**
 * Retrieves paths to man pages by running `manpath` and splitting its
 * stdout into an array. If successful, the result is cached and
 * subsequent calls will return the same result.
 */
size_t get_man_paths_from_manpath_executable(const char * const *paths[])
{
    // Keep the result around for simpler memory management and caching.
    // Entries in `manpath_paths` just point to partial strings in `buffer[]`.
    static char const **manpath_paths = NULL;
    static size_t number_of_manpath_paths = 0;
    static char buffer[MAN_PATHS_BUFFER_SIZE];

    // if `manpath` returned anything prevously, use that
    if (manpath_paths != NULL)
    {
        *paths = manpath_paths;
        return number_of_manpath_paths;
    }

    if(!read_stdout_from_manpath(buffer))
        return 0;

    number_of_manpath_paths = allocating_array_for_paths(buffer, &manpath_paths);
    if(!number_of_manpath_paths)
        return 0;

    split_buffer_into_paths(buffer, &manpath_paths);

    *paths = manpath_paths;
    return number_of_manpath_paths;
}
