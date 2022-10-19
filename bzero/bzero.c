#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>


int main(int argc, char *argv[]){
	
	struct stat statbuf;
	FILE *f;
	
	if (argc != 2){
		printf("Error en el numero de argumentos\n");
		exit(1);
	}
	
	f = fopen(argv[1], "rw");
	stat(f, &statbuf)
	off_t size = statbuf.st_size;
	explicit_bzero(*f, size);
	printf("Reemplazo con ceros realizado (%d bytes)\n", size);
	exit(0);
}