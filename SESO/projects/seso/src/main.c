// Daniel Ruskov y Veselin Solenkov
// Ultima modificacion 01/07/2020
// Ingenieria Informatica UPV/EHU
// Arquitectura y Tecnologia de Computadores
// Sistemas Operativos, 3er curso
//============================================

#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4platsupport/bootinfo.h>

#define FALSE 0
#define TRUE !(FALSE)
#define MAX_MEMORY_REGIONS 512

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//  DEFINIDION GLOBAL DE ESTRUCTURAS, CONSTANTES, VARIABLES Y PUNTEROS
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Registro que define una region de memoria
 * @paddr direccion de inicio de la region
 * @sizeBitsPow tamaño de la region de memoria (2^sizeBits)
 * @isAllocated si esta lible (false) u ocupada (true)
 */
struct Region {
    seL4_Word paddr;
    unsigned int sizeBitsPow;	// si se define como seL4_Uint8 da problema de asignacion
    seL4_Bool isAllocated;
};

/**
 * Lista estatica de regiones (una region = 1..n slots consecutivos)
 * @regions array con las regiones identificadas
 * @countRegions contador de diferentes regiones identificadas
 */
struct Regions {
    struct Region regions[MAX_MEMORY_REGIONS];
    int countRegions;
};

/**
 * Lista estatica con la copia de boot_info->untypedList[] para 
 * ser ordenada por paddr, ya que no se puede modificar la original
 * @myUntypedList array con los slots de memoria libre asignada a
 *                Root_task que cumplen (isDevice == 0)
 * @countSlots contador de los slots de memoria libre
 */
struct Slots {
    seL4_UntypedDesc myUntypedList[CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS];
    int countSlots;
};

const seL4_BootInfo *boot_info;
seL4_Uint8 aligment;
struct Regions maxMemoryRegionAllocates;

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//  FUNCIONES AUXILIARES
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
* Imprime todos los campos de seL4_BootInfo
* @info puntero a estructura de datos que contiene seL4_BootInfo
*/
static void print_bootinfo(const seL4_BootInfo *info) {

	int i;

	/* General info */
	printf("Info Page:\t\t%p\n", info);
	printf("IPC Buffer:\t\t%p\n", info->ipcBuffer);
	printf("Node ID:\t\t%d (of %d)\n", (int)info->nodeID, (int)info->numNodes);
	printf("IOPT levels:\t\t%d\n", (int)info->numIOPTLevels);
	printf("Init cnode size bits:\t%d\n", (int)info->initThreadCNodeSizeBits);
	printf("------------------------------------------------------------\n");
	/* Cap details */
	printf("Cap details:\n");
	printf("Type\t\t\tStart\t\tEnd\n");
	printf("Empty\t\t\t0x%08x\t0x%08x\n", (unsigned int)info->empty.start, (unsigned int)info->empty.end);
	printf("Shared frames\t\t0x%08x\t0x%08x\n", (unsigned int)info->sharedFrames.start, (unsigned int)info->sharedFrames.end);
	printf("User image frames\t0x%08x\t0x%08x\n", (unsigned int)info->userImageFrames.start, (unsigned int)info->userImageFrames.end);
	printf("User image PTs\t\t0x%08x\t0x%08x\n", (unsigned int)info->userImagePaging.start, (unsigned int)info->userImagePaging.end);
	printf("Untypeds\t\t0x%08x\t0x%08x\n", (unsigned int)info->untyped.start, (unsigned int)info->untyped.end);
	/* Untyped details */
	printf("------------------------------------------------------------\n");
	printf("Untyped (info->untypedList[] unsorted) details:\n");
	printf("Untyped\tSlot\t\tPaddr\t\tBits\tDevice\n");
	for (i = 0; i < info->untyped.end-info->untyped.start; i++) {
		if (!(info->untypedList[i].isDevice))
			printf("%3d\t0x%08x\t0x%08x\t%2d\t%d\n", i, (unsigned int)info->untyped.start + i, (unsigned int)info->untypedList[i].paddr, info->untypedList[i].sizeBits, info->untypedList[i].isDevice);
	}
	printf("------------------------------------------------------------\n");
}

/**
 * A utility function to swap two elements
 * @a pointer to first UntypedDesc
 * @b pointer to second UntypedDesc
 */
void swap_UntypedDesc(seL4_UntypedDesc *a, seL4_UntypedDesc *b) {

	seL4_UntypedDesc aux;

	aux.paddr = a->paddr;
	aux.sizeBits = a->sizeBits;
	aux.isDevice = a->isDevice;
	a->paddr = b->paddr;
	a->sizeBits = b->sizeBits;
	a->isDevice = b->isDevice;
	b->paddr = aux.paddr;
	b->sizeBits = aux.sizeBits;
	b->isDevice = aux.isDevice;
}

/**
 * The main function that implements QuickSort
 * @arr[] Array to be sorted
 * @low Starting index
 * @high Ending index
 */
void quickSort_UntypedDesc(seL4_UntypedDesc *arr, int low, int high) {

	int pivot, i, j;

	if (low < high) {
		i = pivot = low;
		j = high;
		while (i < j) {
			while (arr[i].paddr <= arr[pivot].paddr && i < high)
				i++;
			while (arr[j].paddr > arr[pivot].paddr)
				j--;
			if (i < j)
				swap_UntypedDesc(&arr[i], &arr[j]);
		}
		swap_UntypedDesc(&arr[pivot], &arr[j]);
		quickSort_UntypedDesc(arr, low, j - 1);
		quickSort_UntypedDesc(arr, j + 1, high);
	}
}

/**
* Detecta dos regiones consecutivas
* @region1 Dirección de comienzo de la primera region
* @region2 Dirección de comienzo de la segunda region
* @sizeBits tamalo de la region primera
* @return true (!0) si son consecutivas. Si no, false (0)
*/
seL4_Uint8 are_consecutive(seL4_Word region1, seL4_Word region2, seL4_Uint8 sizeBits) {

	if (region1 < region2) {
		if ((region2 - region1) == 2<<(sizeBits-1))
			return TRUE;
		else
			return FALSE;
	} else {
		if ((region1 - region2) == 2<<(sizeBits-1))
        	return TRUE;
  	  	else
        	return FALSE;
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//  FUNCIONES INIT_MEMORY_SYSTEM, ALLOCATE y RELEASE
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Inicializa la memoria asignada a Root_task
 * ordenando untypedList[] y a partir de ella
 * identificando las regiones de memoria libres
 * @aligment alineacion de memoria 8, 16, 32 o 64
 * @return 0 en finalizacion correcta, !0 e.o.c
 */
int init_memory_system(seL4_Uint8 aligment) {

	int i;
	// Crea estructuras auxiliares
	struct Slots myOrderedSlots;
	struct Regions memoryRegions;
	
	// Copia de boot_info->untypedList[] a myOrderedSlots.myUntypedList[], ya que no podemos modificar boot_info->untypedList[]
	myOrderedSlots.countSlots = 0;
	for (i = 0; i < boot_info->untyped.end - boot_info->untyped.start; i++) {
        if (!(boot_info->untypedList[i].isDevice)){
            myOrderedSlots.myUntypedList[myOrderedSlots.countSlots].paddr = boot_info->untypedList[i].paddr;
			myOrderedSlots.myUntypedList[myOrderedSlots.countSlots].sizeBits = boot_info->untypedList[i].sizeBits;
			myOrderedSlots.myUntypedList[myOrderedSlots.countSlots].isDevice = boot_info->untypedList[i].isDevice;
			// No necesario copiar padding1 y padding2, ya que no se usan. 
			// Se puese crear una nueva estructura que los excluya para ahorrar espacio de memoria
			myOrderedSlots.countSlots++;
        }     
    }
    // Ordenar myOrderedSlots->myUntypedList[] e imprimir resultado
    quickSort_UntypedDesc(myOrderedSlots.myUntypedList, 0, myOrderedSlots.countSlots - 1);
	printf("Untyped (myOrderedSlots.myUntypedList[] by paddr) details:\n");
    printf("Untyped\tPaddr\t\tBits\tDevice\t2^Bits\n");
    for (i = 0; i < myOrderedSlots.countSlots; i++) {
        printf("%3d\t0x%08x\t%2d\t%d\t%9d\n", i, (unsigned int)myOrderedSlots.myUntypedList[i].paddr, myOrderedSlots.myUntypedList[i].sizeBits, myOrderedSlots.myUntypedList[i].isDevice, 2<<(myOrderedSlots.myUntypedList[i].sizeBits-1));
    }
	printf("-----------------------------------------------------------\n");
	// Identificar las diferentes regiones (slots contiguos), guardarlos en memoryRegions.regions[] e imprimir resultado. Ademas, la mas grannde guardarla en maxMemoryRegionAllocates.regions[0]
	memoryRegions.countRegions = 0;
	maxMemoryRegionAllocates.regions[0].paddr = 0;
	maxMemoryRegionAllocates.regions[0].sizeBitsPow = 0;
	maxMemoryRegionAllocates.regions[0].isAllocated = 0;
	maxMemoryRegionAllocates.countRegions = 1;
	// copia primer slot
	memoryRegions.regions[memoryRegions.countRegions].paddr = myOrderedSlots.myUntypedList[0].paddr;
	memoryRegions.regions[memoryRegions.countRegions].sizeBitsPow = 2<<(myOrderedSlots.myUntypedList[0].sizeBits-1);
	memoryRegions.regions[memoryRegions.countRegions].isAllocated = FALSE;
	// para todos los slots
	for (i=1; i<myOrderedSlots.countSlots; i++) {
		// si es contiguo al anterior, sumar los 2^sizeBits de la anterior
		if (are_consecutive(myOrderedSlots.myUntypedList[i-1].paddr, myOrderedSlots.myUntypedList[i].paddr, myOrderedSlots.myUntypedList[i-1].sizeBits)) {
			memoryRegions.regions[memoryRegions.countRegions].sizeBitsPow += 2<<(myOrderedSlots.myUntypedList[i].sizeBits-1);
		} else {
			// Si la region nueva es mayor que la mas grande hasta el momento, copiarla
			if (memoryRegions.regions[memoryRegions.countRegions].sizeBitsPow > maxMemoryRegionAllocates.regions[0].sizeBitsPow) {
				maxMemoryRegionAllocates.regions[0].paddr = memoryRegions.regions[memoryRegions.countRegions].paddr;
				maxMemoryRegionAllocates.regions[0].sizeBitsPow = memoryRegions.regions[memoryRegions.countRegions].sizeBitsPow;
			}
			// iniciar la siguiente region
			memoryRegions.countRegions++;
			memoryRegions.regions[memoryRegions.countRegions].paddr = myOrderedSlots.myUntypedList[i].paddr;
			memoryRegions.regions[memoryRegions.countRegions].sizeBitsPow = 2<<(myOrderedSlots.myUntypedList[i].sizeBits-1);
			memoryRegions.regions[memoryRegions.countRegions].isAllocated = FALSE;
		}
	}
	// si la ultima region es la mas grande, guardarla
	if (memoryRegions.regions[memoryRegions.countRegions].sizeBitsPow > maxMemoryRegionAllocates.regions[0].sizeBitsPow) {
		maxMemoryRegionAllocates.regions[0].paddr = memoryRegions.regions[memoryRegions.countRegions].paddr;
		maxMemoryRegionAllocates.regions[0].sizeBitsPow = memoryRegions.regions[memoryRegions.countRegions].sizeBitsPow;
	}
	memoryRegions.countRegions++;
	//printf("Numero de regiones: %d\n", memoryRegions.countRegions);
	//printf("Máxima región: 0x%08lx, %d\n", maxMemoryRegionAllocates.regions[0].paddr, maxMemoryRegionAllocates.regions[0].sizeBitsPow);
	printf("Regions (memoryRegions.regions[] unsorted) details:\n");
    printf("Region\tisAllocated\tPaddr\t\t2^Bits\t\n");
    for (i = 0; i < memoryRegions.countRegions; i++) {
        printf("%3d\t%2d\t\t0x%08x\t%9d\n", i, memoryRegions.regions[i].isAllocated, (unsigned int)memoryRegions.regions[i].paddr, memoryRegions.regions[i].sizeBitsPow);
    }
	printf("------------------------------------------------------------\n");
	return 0;
}

/**
 * Reserva la primera region de memoria alineada de tamaño 2^sizeBits
 * Politica firs fit
 * @sizeBits tamaño de memoria a reservar
 * @return puntero a la region de memoria reservada, 0 e.o.c con msg de error
 */
seL4_Word allocate(seL4_Uint8 sizeBits) {

	int i = 0, j;
	seL4_Word mask, paddr;
	unsigned int sizeBitsPow = 2<<(sizeBits-1);

	// define mascara a usar
	if (aligment == 64)
		mask = 7;
	else if (aligment == 32)
		mask = 3;
	else if (aligment == 16)
		mask = 1;
	else
		mask = 0;
	// mientras quedan regiones posibles
	while (i<maxMemoryRegionAllocates.countRegions) {
		// buscar la rimera region libre (first fit con isAllocated = false) 
		while (maxMemoryRegionAllocates.regions[i].isAllocated || maxMemoryRegionAllocates.regions[i].sizeBitsPow < sizeBitsPow)
			i++;
		// coger si direccion de memoria
		paddr = maxMemoryRegionAllocates.regions[i].paddr;
		// mientras no este alineado, avanzar la direccion
		while ((paddr & mask) != 0)
			paddr++;
		// Con direccion alineada, si hay espacio suficiente para reservar, efectuar reserva
		if ((paddr - maxMemoryRegionAllocates.regions[i].paddr) <= (maxMemoryRegionAllocates.regions[i].sizeBitsPow - sizeBitsPow)) {
			// si la direccion de inicio de region es alineada
			if (paddr == maxMemoryRegionAllocates.regions[i].paddr) {
				// si es de tamaño exacto la refion y el solicitado, no quedan restos libres de la region
				if (maxMemoryRegionAllocates.regions[i].sizeBitsPow == sizeBitsPow){
					// marcar como allocated y devolver el puntero, no hace falta trocear |region i tamano 2^sizeBits reservada|
					maxMemoryRegionAllocates.regions[i].isAllocated = TRUE;
				} else {
					// dividir en la region a reservar y el resto de la region libre |region i tamano 2^sizeBits reservada|nueva region i+1 del resto (maxMemoryRegionAllocates.regions[i].sizeBitsPow - 2^sizeBits) no reservada|
					for (j = maxMemoryRegionAllocates.countRegions; j>i+1; j--) {
						// copia de las regiones siguientes una posicion adelante
						maxMemoryRegionAllocates.regions[j].paddr = maxMemoryRegionAllocates.regions[j-1].paddr;
						maxMemoryRegionAllocates.regions[j].sizeBitsPow = maxMemoryRegionAllocates.regions[j-1].sizeBitsPow;
						maxMemoryRegionAllocates.regions[j].isAllocated = maxMemoryRegionAllocates.regions[j-1].isAllocated;
					}
					// region resto a la derecha
					maxMemoryRegionAllocates.regions[i+1].paddr = maxMemoryRegionAllocates.regions[i].paddr + sizeBitsPow;
					maxMemoryRegionAllocates.regions[i+1].sizeBitsPow = maxMemoryRegionAllocates.regions[i].sizeBitsPow - sizeBitsPow;
					maxMemoryRegionAllocates.regions[i+1].isAllocated = FALSE;
					// region reservada
					//maxMemoryRegionAllocates.regions[i+1].paddr = paddr; //same
					maxMemoryRegionAllocates.regions[i].sizeBitsPow = sizeBitsPow;
					maxMemoryRegionAllocates.regions[i].isAllocated = TRUE;
					// aumenta contador de regiones
					maxMemoryRegionAllocates.countRegions++;
				}
			} else if ((paddr - maxMemoryRegionAllocates.regions[i].paddr) == (maxMemoryRegionAllocates.regions[i].sizeBitsPow - sizeBitsPow)) {
				// la seccion a reservar esta desde un punto medio de la region hasta el final, dejando resto parte libre a la izda
				// |region i del resto (maxMemoryRegionAllocates.regions[i].sizeBitsPow - 2^sizeBits) no reservada|nueva region i+1 tamano 2^sizeBits reservada|
				for (j = maxMemoryRegionAllocates.countRegions; j>i+1; j--) {
					// copia de las regiones siguientes una posicion adelante
					maxMemoryRegionAllocates.regions[j].paddr = maxMemoryRegionAllocates.regions[j-1].paddr;
					maxMemoryRegionAllocates.regions[j].sizeBitsPow = maxMemoryRegionAllocates.regions[j-1].sizeBitsPow;
					maxMemoryRegionAllocates.regions[j].isAllocated = maxMemoryRegionAllocates.regions[j-1].isAllocated;
				}
				// region reservada
				maxMemoryRegionAllocates.regions[i+1].paddr = paddr;
				maxMemoryRegionAllocates.regions[i+1].sizeBitsPow = sizeBitsPow;
				maxMemoryRegionAllocates.regions[i+1].isAllocated = TRUE;
				// region resto a la izda
				maxMemoryRegionAllocates.regions[i].sizeBitsPow -= sizeBitsPow;
				maxMemoryRegionAllocates.regions[i].isAllocated = FALSE;
				// aumenta contador de regiones
				maxMemoryRegionAllocates.countRegions++;
			} else {
				// la seccion a reservar es un trozo en punto medio de la region con restos libres a la izda y dcha
				// |region i del resto (maxMemoryRegionAllocates.regions[i].sizeBitsPow - 2^sizeBits - [i+2].sizeBitsPow) no reservada|nueva region i+1 tamano 2^sizeBits reservada|region i+2 del resto (maxMemoryRegionAllocates.regions[i].sizeBitsPow - 2^sizeBits - [i].sizeBitsPow) no reservada|
				for (j = maxMemoryRegionAllocates.countRegions+1; j>i+2; j--) {
					// copia de las regiones siguientes dos posiciones adelante
					maxMemoryRegionAllocates.regions[j].paddr = maxMemoryRegionAllocates.regions[j-2].paddr;
					maxMemoryRegionAllocates.regions[j].sizeBitsPow = maxMemoryRegionAllocates.regions[j-2].sizeBitsPow;
					maxMemoryRegionAllocates.regions[j].isAllocated = maxMemoryRegionAllocates.regions[j-2].isAllocated;
				}
				// region resto a la derecha
				maxMemoryRegionAllocates.regions[i+2].paddr = paddr + sizeBitsPow;
				maxMemoryRegionAllocates.regions[i+2].sizeBitsPow = maxMemoryRegionAllocates.regions[i].sizeBitsPow - (maxMemoryRegionAllocates.regions[i+2].paddr - maxMemoryRegionAllocates.regions[i].paddr);
				maxMemoryRegionAllocates.regions[i+2].isAllocated = FALSE;
				// region reservada
				maxMemoryRegionAllocates.regions[i+1].paddr = paddr;
				maxMemoryRegionAllocates.regions[i+1].sizeBitsPow = sizeBitsPow;
				maxMemoryRegionAllocates.regions[i+1].isAllocated = TRUE;
				// region resto a la izda
				maxMemoryRegionAllocates.regions[i].sizeBitsPow = maxMemoryRegionAllocates.regions[i].sizeBitsPow - sizeBitsPow - maxMemoryRegionAllocates.regions[i+2].sizeBitsPow;
				maxMemoryRegionAllocates.regions[i].isAllocated = FALSE;
				// aumenta contador de regiones
				maxMemoryRegionAllocates.countRegions += 2;
			}
			return paddr;
		} else {
			// seguir buscando a partir de la siguiente region si existe
			i++;
		}
	}
	printf("ERROR: No se ha podido efectuar la reserva de memoria allocate(%d)\n", (int) sizeBits);
	return 0;
}

/**
 * Libera la region de memoria apuntada por paddr
 * @paddr puntero al inicio de la region de memoria a librerar
 * @return 0 en ejecucion correcta, cogigo de error e.o.c
 */
int release(seL4_Word paddr) {

	int i = 0, j;

	// avanzar hasta encontrar la region reservada a liberar dentro de maxMemoryRegionAllocates.regions[]
	while (paddr != maxMemoryRegionAllocates.regions[i].paddr && i < maxMemoryRegionAllocates.countRegions)
		i++;
	// si no encuentra esa region, error
	if (i == maxMemoryRegionAllocates.countRegions) {
		printf("ERROR: El puntero 0x%08x no pertenece a ninguna region\n", (unsigned int) paddr);
		return 1;
	}
	// si la region estaya libre, error
	if (!maxMemoryRegionAllocates.regions[i].isAllocated) {
		printf("ERROR: El puntero 0x%08x pertenece region libre\n", (unsigned int) paddr);
		return 2;
	}
	// si es la primera region de maxMemoryRegionAllocates.regions[]
	if (i == 0) {
		if (maxMemoryRegionAllocates.countRegions > 1 && !maxMemoryRegionAllocates.regions[i+1].isAllocated) { // |i=0 reservado|i+1 libre|...|
			// juntar regiones i, i+1
			maxMemoryRegionAllocates.regions[i].sizeBitsPow += maxMemoryRegionAllocates.regions[i+1].sizeBitsPow;
			maxMemoryRegionAllocates.regions[i].isAllocated = FALSE;
			maxMemoryRegionAllocates.countRegions--;
			for (j=i+1; j<maxMemoryRegionAllocates.countRegions; j++) {
				// mueve de las regiones siguientes una posicion atras
				maxMemoryRegionAllocates.regions[j].paddr = maxMemoryRegionAllocates.regions[j+1].paddr;
				maxMemoryRegionAllocates.regions[j].sizeBitsPow = maxMemoryRegionAllocates.regions[j+1].sizeBitsPow;
				maxMemoryRegionAllocates.regions[j].isAllocated = maxMemoryRegionAllocates.regions[j+1].isAllocated;
			}
		} else { // |i=0 reservado|i+1 reservado|...| o // |i=0 reservado|...vacio...|
			maxMemoryRegionAllocates.regions[i].isAllocated = FALSE;
		}
	} else if (i == maxMemoryRegionAllocates.countRegions-1) {
		// si es la ultima region de maxMemoryRegionAllocates.regions[]
		if (!maxMemoryRegionAllocates.regions[i-1].isAllocated) { // |...|i-1 libre|i reservado|
			// juntar regiones i-1, i
			maxMemoryRegionAllocates.regions[i-1].sizeBitsPow += maxMemoryRegionAllocates.regions[i].sizeBitsPow;
			maxMemoryRegionAllocates.countRegions--;
		} else { // |...|i-1 reservado|i reservado|
			maxMemoryRegionAllocates.regions[i].isAllocated = FALSE;
		}
	} else {
		// si es una region intermedia
		// si la izda y la dcha estan reservadas, liberar y terminar
		if (maxMemoryRegionAllocates.regions[i-1].isAllocated && maxMemoryRegionAllocates.regions[i+1].isAllocated) { // |...|i-1 reservado|i reservado|i+1 reservado|...|
			maxMemoryRegionAllocates.regions[i].isAllocated = FALSE;
		} else if (!maxMemoryRegionAllocates.regions[i-1].isAllocated && !maxMemoryRegionAllocates.regions[i+1].isAllocated) { // |...|i-1 libre|i reservado|i+1 libre|...|
			// si la izda y la dcha estan libres, liberar, juntar y terminar
			// sumar espacio de las tres regiones
			maxMemoryRegionAllocates.regions[i-1].sizeBitsPow = maxMemoryRegionAllocates.regions[i-1].sizeBitsPow + maxMemoryRegionAllocates.regions[i].sizeBitsPow + maxMemoryRegionAllocates.regions[i+1].sizeBitsPow;
			// reducir contador de regiones en 2
			maxMemoryRegionAllocates.countRegions -= 2;
			for (j=i; j<maxMemoryRegionAllocates.countRegions; j++) {
				// mueve de las regiones siguientes dos posiciones atras
				maxMemoryRegionAllocates.regions[j].paddr = maxMemoryRegionAllocates.regions[j+2].paddr;
				maxMemoryRegionAllocates.regions[j].sizeBitsPow = maxMemoryRegionAllocates.regions[j+2].sizeBitsPow;
				maxMemoryRegionAllocates.regions[j].isAllocated = maxMemoryRegionAllocates.regions[j+2].isAllocated;
			}
		} else if (maxMemoryRegionAllocates.regions[i-1].isAllocated && !maxMemoryRegionAllocates.regions[i+1].isAllocated) { // |...|i-1 reservado|i reservado|i+1 libre|...|
			// juntar regiones i, i+1
			maxMemoryRegionAllocates.regions[i].sizeBitsPow += maxMemoryRegionAllocates.regions[i+1].sizeBitsPow;
			maxMemoryRegionAllocates.regions[i].isAllocated = FALSE;
			maxMemoryRegionAllocates.countRegions--;
			for (j=i+1; j<maxMemoryRegionAllocates.countRegions; j++) {
				// mueve de las regiones siguientes una posicion atras
				maxMemoryRegionAllocates.regions[j].paddr = maxMemoryRegionAllocates.regions[j+1].paddr;
				maxMemoryRegionAllocates.regions[j].sizeBitsPow = maxMemoryRegionAllocates.regions[j+1].sizeBitsPow;
				maxMemoryRegionAllocates.regions[j].isAllocated = maxMemoryRegionAllocates.regions[j+1].isAllocated;
			}
		} else { // |...|i-1 libre|i reservado|i+1 reservado|...|
			// juntar regiones i-1, i
			maxMemoryRegionAllocates.regions[i-1].sizeBitsPow += maxMemoryRegionAllocates.regions[i].sizeBitsPow;
			maxMemoryRegionAllocates.regions[i-1].isAllocated = FALSE;
			maxMemoryRegionAllocates.countRegions--;
			for (j=i; j<maxMemoryRegionAllocates.countRegions; j++) {
				// mueve de las regiones siguientes una posicion atras
				maxMemoryRegionAllocates.regions[j].paddr = maxMemoryRegionAllocates.regions[j+1].paddr;
				maxMemoryRegionAllocates.regions[j].sizeBitsPow = maxMemoryRegionAllocates.regions[j+1].sizeBitsPow;
				maxMemoryRegionAllocates.regions[j].isAllocated = maxMemoryRegionAllocates.regions[j+1].isAllocated;
			}
		}
	}
	return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//  MAIN - PRUEBAS DE EJECUCION
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Programa de pruebas
 * @return 0 en ejecucion correcta, !0 e.o.c
 */
int main(void) {

	int i;

    printf("\n\n>>>\n>>> Welcome to the SESO operating system\n>>>\n");
    printf("============================================================\n");

    boot_info = platsupport_get_bootinfo();
    print_bootinfo(boot_info);
    
    aligment = 64;               // Aineacion 8, 16, 32 o 64
    init_memory_system(aligment);

	printf("Aligment: %d\n", aligment);
	seL4_Word paddr1 = allocate(6);
	printf("allocate(6): 0x%08x\n", (unsigned int) paddr1);
	seL4_Word paddr2 = allocate(12);
	printf("allocate(12): 0x%08x\n", (unsigned int) paddr2);
	seL4_Word paddr3 = allocate(4);
	printf("allocate(4): 0x%08x\n", (unsigned int) paddr3);
	printf("------------------------------\n");
	printf("Regions (maxMemoryRegionAllocates.regions[]) details:\n");
    printf("Region\tisAllocated\tPaddr\t\t2^Bits\t\n");
    for (i = 0; i < maxMemoryRegionAllocates.countRegions; i++) {
        printf("%3d\t%2d\t\t0x%08x\t%9d\n", i, maxMemoryRegionAllocates.regions[i].isAllocated, (unsigned int) maxMemoryRegionAllocates.regions[i].paddr, maxMemoryRegionAllocates.regions[i].sizeBitsPow);
    }
	printf("------------------------------------------------------------\n");

	// release en ese orden para probar todos los casos posibles
	printf("release(0x%08x)\n", (unsigned int) maxMemoryRegionAllocates.regions[maxMemoryRegionAllocates.countRegions-1].paddr);
	release(maxMemoryRegionAllocates.regions[maxMemoryRegionAllocates.countRegions-1].paddr);
	printf("release(0x%08x)\n", (unsigned int) paddr2);
	release(paddr2);
/*	printf("Regions (maxMemoryRegionAllocates.regions[]) details:\n");
    printf("Region\tisAllocated\tPaddr\t\t2^Bits\t\n");
    for (i = 0; i < maxMemoryRegionAllocates.countRegions; i++) {
        printf("%3d\t%2d\t\t0x%08x\t%9d\n", i, maxMemoryRegionAllocates.regions[i].isAllocated, (unsigned int) maxMemoryRegionAllocates.regions[i].paddr, maxMemoryRegionAllocates.regions[i].sizeBitsPow);
    }
	printf("------------------------------------------------------------\n");
*/	printf("release(0x%08x)\n", (unsigned int) paddr3);
	release(paddr3);
/*	printf("Regions (maxMemoryRegionAllocates.regions[]) details:\n");
    printf("Region\tisAllocated\tPaddr\t\t2^Bits\t\n");
    for (i = 0; i < maxMemoryRegionAllocates.countRegions; i++) {
        printf("%3d\t%2d\t\t0x%08x\t%9d\n", i, maxMemoryRegionAllocates.regions[i].isAllocated, (unsigned int) maxMemoryRegionAllocates.regions[i].paddr, maxMemoryRegionAllocates.regions[i].sizeBitsPow);
    }
	printf("------------------------------------------------------------\n");
*/	printf("release(0x%08x)\n", (unsigned int) paddr1);
	release(paddr1);
	printf("release(0x%08x)\n", (unsigned int) paddr1+1);
	release(paddr1+1);
	printf("------------------------------\n");
	printf("Regions (maxMemoryRegionAllocates.regions[]) details:\n");
    printf("Region\tisAllocated\tPaddr\t\t2^Bits\t\n");
    for (i = 0; i < maxMemoryRegionAllocates.countRegions; i++) {
        printf("%3d\t%2d\t\t0x%08x\t%9d\n", i, maxMemoryRegionAllocates.regions[i].isAllocated, (unsigned int) maxMemoryRegionAllocates.regions[i].paddr, maxMemoryRegionAllocates.regions[i].sizeBitsPow);
    }
	printf("------------------------------------------------------------\n");

    printf("============================================================\n");
    printf(">>> See you soon!\n\n");

    return 0;
}
