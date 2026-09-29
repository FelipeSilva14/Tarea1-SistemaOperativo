# Tarea 1: Planificador de Tareas - Sistemas Operativos

## Integrantes
* Francisco San Martín
* Felipe Silva

## Descripción del Proyecto
Este proyecto implementa un **planificador de tareas concurrentes** en C, capaz de gestionar la ejecución de procesos organizados. El sistema lee una lista de tareas desde un archivo de texto (`plan.txt`), determina sus dependencias y las ejecuta de forma paralela respetando un límite máximo de $K$ procesos simultáneos, utilizando procesos (`fork`) y tuberías (`pipes`) para la comunicación.

---

## Requisitos e Instalación

### Requisitos del Sistema
* OS: Linux (Ubuntu 20.04 LTS o superior).
* Compilador: `gcc`.

### Compilación
Para compilar el proyecto se creo un Makefile por lo que se debe ejecutar en la terminal el siguiente comando estando en la carpeta con todos los archivos descargados:

```bash
make
```
## Modo de uso
El ejecutable recibe dos argumentos principales: la ruta al archivo de tareas (plan.txt) y un entero positivo $K$ que define el grado máximo de concurrencia de procesos:

```bash
./planificador <archivo_plan> <K>

En este caso se utiliza ./planificador plan.txt 1000
```

## Formato del archivo plan.txt
Cada línea del archivo representa una tarea con la siguiente estructura: `ID:nombre:duración_ms:dependencias`

`ID`: Es el identificador único de la tarea.

`Nombre`: Nombre de la tarea.

`Duración_ms`: Tiempo de simulación en milisegundos (si está vacío, se le asignara un tiempo aleatorio entre 100 ms y 5000 ms).

`Dependencias`: Lista separada por comas de IDs de tareas previas que deben FINALIZAR antes de iniciar la tarea actual.

Ejemplo:

`1:prender_carbon:500:`

`2:comprar_carne:1200:`

`3:comprar_pan:300:`

`4:comprar_bebestibles:400:`

`5:preparar_pebre:600:`

`6:asar_longaniza:800:1,2`

## Funciones implementadas
  
* `Leer_archivo(const char *filename)`: Esto parsea el archivo .txt utilizando fgets y sscanf. Construye los nodos de la estructura NodoTarea y extrae dependencias separadas por comas utilizando strtok.

* `encontrar_id(const char *id)`: Esta función realiza una búsqueda secuencial en la estructura del grafo para retornar el índice correspondiente.  

* `hacer_tarea(NodoTarea *node)`: Función ejecutada exclusivamente por los procesos hijos generados a través de fork(). Simula la ejecución de la tarea durmiendo el proceso durante duracion_ms mediante usleep(). Notifica su finalización enviando el mensaje COMPLETADO:<ID> al proceso padre mediante un pipe.  

* `notify_dependents(const char *completed_id)`: Recorre las tareas pendientes del grafo reduciendo el contador de dependencias no resueltas de los nodos hijos cuando la tarea padre finaliza de manera correcta.  
 
* `cerrar_rama(void)`: Sirve para el aislamiento de errores. Propaga en cascada el estado de FALLO a todos los nodos descendientes si una tarea padre falla o es cancelada.  

* `inspeccion_seremi(int sig)`: Esta función es el manejador de señal SIGINT (Ctrl+C). Termina de forma limpia y cancela inmediatamente todos los procesos hijos en ejecución enviándoles SIGTERM.

## Decisiones de diseño tomadas

* `1) Procesos o Hilos`: Para cumplir con la rúbrica de la tarea no se realizó el uso de hilos de ejecución para esta tarea. Toda la concurrencia está basada en la creación de procesos independientes utilizando fork() y sincronización mediante waitpid(-1, &estado, 0).  

* `2) Paso de Mensajes (Pipes)`: Cada tarea crea una tubería unidireccional (pipe) antes de realizar el fork(). Esto ayuda a que se garantice la comunicación de finalización entre el proceso hijo y el padre de una manera segura y desacoplada.

* `3) Prevención de Busy-Waiting (Espera Activa)`: El bucle principal del planificador en el proceso padre utiliza waitpid(-1, ...) bloqueante. Esto le permite ceder el control del CPU al sistema operativo mientras los procesos hijos ejecutan su trabajo, reduciendo así el consumo ineficiente de los distintos recursos.

* `4) Gestión Dinámica de Memoria`: Se utilizó asignación dinámica con malloc y realloc tanto para el grafo principal como para las listas de dependencias, dejando que se permita escalar a archivos con una carga de tareas mayor.

