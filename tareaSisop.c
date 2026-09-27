#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

#define MAX_ID_LEN 64
#define MAX_NAME_LEN 64

typedef enum { PENDIENTE, EJECUTANDOSE, COMPLETADO, FALLO } TipoEstado;

typedef struct {
    char id[MAX_ID_LEN];
    char name[MAX_NAME_LEN];
    int duracion_ms;
    
    char **id_padre;
    int contador_padres;
    
    int dependencias;
    TipoEstado estado;
    pid_t pid;
    int pipe_fd[2];
} NodoTarea;

NodoTarea *graph = NULL;
int contador_nodo = 0;
int capacidad = 0;

// Busca el índice de una tarea en el grafo a partir de su ID alfanumérico
int encontrar_id(const char *id) {
    for (int i = 0; i < contador_nodo; i++) {
        if (strcmp(graph[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

void inspeccion_seremi(int sig) {
    (void)sig;
    printf("\n[SEREMI] Inspección Seremi. Se cancelan todas las actividades.\n");
    for (int i = 0; i < contador_nodo; i++) {
        if (graph[i].estado == EJECUTANDOSE && graph[i].pid > 0) {
            kill(graph[i].pid, SIGTERM);
        }
    }
    exit(EXIT_FAILURE);
}

int obtener_duracion_random(void) {
    return (rand() % 4901) + 100; // en esta parte es donde se le otorga 100 ms a 5000 ms de manera random, por si no se le asigna
}

// esto comprueba que todas las dependencias en el archivo existan en el grafo
int validar_grafo(void) {
    for (int i = 0; i < contador_nodo; i++) {
        for (int j = 0; j < graph[i].contador_padres; j++) {
            if (encontrar_id(graph[i].id_padre[j]) == -1) {
                fprintf(stderr, "[ERROR] La tarea '%s' depende de '%s', pero esa dependencia no existe o fallo.\n", 
                        graph[i].id, graph[i].id_padre[j]);
                return 0; // Grafo inválido
            }
        }
    }
    return 1;
}

void Leer_archivo(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error al abrir el archivo de plan");
        exit(EXIT_FAILURE);
    }

    capacidad = 100;
    graph = malloc(capacidad * sizeof(NodoTarea));

    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;

        if (contador_nodo >= capacidad) {
            capacidad *= 2;
            graph = realloc(graph, capacidad * sizeof(NodoTarea));
        }

        NodoTarea *node = &graph[contador_nodo];
        node->id_padre = NULL;
        node->contador_padres = 0;
        node->estado = PENDIENTE;
        node->pid = -1;

        line[strcspn(line, "\r\n")] = 0; //esto elimina los saltos de linea, para evitar errores

        char id_str[MAX_ID_LEN] = {0};
        char nombre_str[MAX_NAME_LEN] = {0};
        char duracion_str[32] = {0};
        char dependencias_str[512] = {0};

        int fields = sscanf(line, "%63[^:]:%63[^:]:%31[^:]:%511s", id_str, nombre_str, duracion_str, dependencias_str);
        //separa las cosas, el id, el nombre, duración y las dependencias

        if (fields < 2) continue;

        // Limpia espacios en blanco
        sscanf(id_str, " %63s", node->id);
        sscanf(nombre_str, " %63[^\n\r]", node->name);

        if (fields >= 3 && strlen(duracion_str) > 0 && atoi(duracion_str) > 0) {
            node->duracion_ms = atoi(duracion_str);
        } else {
            node->duracion_ms = obtener_duracion_random();
        }

        if (fields == 4 && strlen(dependencias_str) > 0) {
            int parent_cap = 5;
            node->id_padre = malloc(parent_cap * sizeof(char *));
            
            char *dep = strtok(dependencias_str, ","); //apunta al primer elemento en una lista de comas
            while (dep) {
                if (node->contador_padres >= parent_cap) {
                    parent_cap *= 2;
                    node->id_padre = realloc(node->id_padre, parent_cap * sizeof(char *));
                }
                
                // remueve espacios en las depencencias
                char clean_dep[MAX_ID_LEN] = {0};
                sscanf(dep, " %63s", clean_dep);
                
                node->id_padre[node->contador_padres] = strdup(clean_dep);
                node->contador_padres++;
                dep = strtok(NULL, ",");
            }
        }

        node->dependencias = node->contador_padres;
        contador_nodo++;
    }
    fclose(file);
}

void hacer_tarea(NodoTarea *node) {
    signal(SIGINT, SIG_DFL);

    close(node->pipe_fd[0]); // cierra el extremo de lectura en el hijo
    printf("[INICIO] Tarea %s (%s) - Duración: %d ms\n", node->id, node->name, node->duracion_ms);

    usleep(node->duracion_ms * 1000);

    char msg[128];
    snprintf(msg, sizeof(msg), "COMPLETADO:%s", node->id);
    
    // aca se transmiten los mensajes a través de Pipe
    if (write(node->pipe_fd[1], msg, strlen(msg)) == -1) {
        perror("Error escribiendo en pipe");
    }
    
    close(node->pipe_fd[1]);
    printf("[FIN] Tarea %s (%s) completada.\n", node->id, node->name);
    exit(EXIT_SUCCESS);
}

// Propagación iterativa para evitar overflow con grafos grandes
void cerrar_rama(void) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < contador_nodo; i++) {
            if (graph[i].estado == PENDIENTE) {
                for (int j = 0; j < graph[i].contador_padres; j++) {
                    int parent_idx = encontrar_id(graph[i].id_padre[j]);
                    if (parent_idx != -1 && graph[parent_idx].estado == FALLO) {
                        graph[i].estado = FALLO;
                        printf("[ABORTADO] Tarea %s abortada por fallo en dependencia %s.\n", graph[i].id, graph[i].id_padre[j]);
                        changed = 1;
                        break;
                    }
                }
            }
        }
    }
}

void notify_dependents(const char *COMPLETADO_id) {
    for (int i = 0; i < contador_nodo; i++) {
        if (graph[i].estado == PENDIENTE) {
            for (int j = 0; j < graph[i].contador_padres; j++) {
                if (strcmp(graph[i].id_padre[j], COMPLETADO_id) == 0) {
                    graph[i].dependencias--;
                }
            }
        }
    }
}

void imprimir_resumen(void) {
    int completadas = 0;
    int fallidas = 0;

    for (int i = 0; i < contador_nodo; i++) {
        if (graph[i].estado == COMPLETADO) {
            completadas++;
        } else if (graph[i].estado == FALLO) {
            fallidas++;
        }
    }

    printf("\nResumen: %d tareas procesadas (%d exitosas, %d canceladas).\n", 
           contador_nodo, completadas, fallidas);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s plan.txt K\n", argv[0]);
        return EXIT_FAILURE;
    }

    int K = atoi(argv[2]);//de texto a entero
    if (K <= 0) {
        fprintf(stderr, "El número K debe ser un entero positivo mayor a 0.\n");
        return EXIT_FAILURE;
    }

    srand(time(NULL));

    struct sigaction sa;                   //todo esto es cuando se presiona CTRL+C y que eso llame a la función
    sa.sa_handler = inspeccion_seremi;     //inspeccion_seremi, de manera que la ejecución se detiene
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    Leer_archivo(argv[1]);
    
    // Validació#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

#define MAX_ID_LEN 64
#define MAX_NAME_LEN 64

typedef enum { PENDIENTE, EJECUTANDOSE, COMPLETADO, FALLO } TipoEstado;

typedef struct {
    char id[MAX_ID_LEN];
    char name[MAX_NAME_LEN];
    int duracion_ms;
    
    char **id_padre;
    int contador_padres;
    
    int dependencias;
    TipoEstado estado;
    pid_t pid;
    int pipe_fd[2];
} NodoTarea;

NodoTarea *graph = NULL;
int contador_nodo = 0;
int capacidad = 0;

// Busca el índice de una tarea en el grafo a partir de su ID alfanumérico
int encontrar_id(const char *id) {
    for (int i = 0; i < contador_nodo; i++) {
        if (strcmp(graph[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

void inspeccion_seremi(int sig) {
    (void)sig;
    printf("\n[SEREMI] Inspección Seremi. Se cancelan todas las actividades.\n");
    for (int i = 0; i < contador_nodo; i++) {
        if (graph[i].estado == EJECUTANDOSE && graph[i].pid > 0) {
            kill(graph[i].pid, SIGTERM);
        }
    }
    exit(EXIT_FAILURE);
}

int obtener_duracion_random(void) {
    return (rand() % 4901) + 100; // en esta parte es donde se le otorga 100 ms a 5000 ms de manera random, por si no se le asigna
}

// esto comprueba que todas las dependencias en el archivo existan en el grafo
int validar_grafo(void) {
    for (int i = 0; i < contador_nodo; i++) {
        for (int j = 0; j < graph[i].contador_padres; j++) {
            if (encontrar_id(graph[i].id_padre[j]) == -1) {
                fprintf(stderr, "[ERROR] La tarea '%s' depende de '%s', pero esa dependencia no existe o fallo.\n", 
                        graph[i].id, graph[i].id_padre[j]);
                return 0; // Grafo inválido
            }
        }
    }
    return 1;
}

void Leer_archivo(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error al abrir el archivo de plan");
        exit(EXIT_FAILURE);
    }

    capacidad = 100;
    graph = malloc(capacidad * sizeof(NodoTarea));

    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;

        if (contador_nodo >= capacidad) {
            capacidad *= 2;
            graph = realloc(graph, capacidad * sizeof(NodoTarea));
        }

        NodoTarea *node = &graph[contador_nodo];
        node->id_padre = NULL;
        node->contador_padres = 0;
        node->estado = PENDIENTE;
        node->pid = -1;

        line[strcspn(line, "\r\n")] = 0; //esto elimina los saltos de linea, para evitar errores

        char id_str[MAX_ID_LEN] = {0};
        char nombre_str[MAX_NAME_LEN] = {0};
        char duracion_str[32] = {0};
        char dependencias_str[512] = {0};

        int fields = sscanf(line, "%63[^:]:%63[^:]:%31[^:]:%511s", id_str, nombre_str, duracion_str, dependencias_str);
        //separa las cosas, el id, el nombre, duración y las dependencias

        if (fields < 2) continue;

        // Limpia espacios en blanco
        sscanf(id_str, " %63s", node->id);
        sscanf(nombre_str, " %63[^\n\r]", node->name);

        if (fields >= 3 && strlen(duracion_str) > 0 && atoi(duracion_str) > 0) {
            node->duracion_ms = atoi(duracion_str);
        } else {
            node->duracion_ms = obtener_duracion_random();
        }

        if (fields == 4 && strlen(dependencias_str) > 0) {
            int parent_cap = 5;
            node->id_padre = malloc(parent_cap * sizeof(char *));
            
            char *dep = strtok(dependencias_str, ","); //apunta al primer elemento en una lista de comas
            while (dep) {
                if (node->contador_padres >= parent_cap) {
                    parent_cap *= 2;
                    node->id_padre = realloc(node->id_padre, parent_cap * sizeof(char *));
                }
                
                // remueve espacios en las depencencias
                char clean_dep[MAX_ID_LEN] = {0};
                sscanf(dep, " %63s", clean_dep);
                
                node->id_padre[node->contador_padres] = strdup(clean_dep);
                node->contador_padres++;
                dep = strtok(NULL, ",");
            }
        }

        node->dependencias = node->contador_padres;
        contador_nodo++;
    }
    fclose(file);
}

void hacer_tarea(NodoTarea *node) {
    signal(SIGINT, SIG_DFL);

    close(node->pipe_fd[0]); // cierra el extremo de lectura en el hijo
    printf("[INICIO] Tarea %s (%s) - Duración: %d ms\n", node->id, node->name, node->duracion_ms);

    usleep(node->duracion_ms * 1000);

    char msg[128];
    snprintf(msg, sizeof(msg), "COMPLETADO:%s", node->id);
    
    // aca se transmiten los mensajes a través de Pipe
    if (write(node->pipe_fd[1], msg, strlen(msg)) == -1) {
        perror("Error escribiendo en pipe");
    }
    
    close(node->pipe_fd[1]);
    printf("[FIN] Tarea %s (%s) completada.\n", node->id, node->name);
    exit(EXIT_SUCCESS);
}

// Propagación iterativa para evitar overflow con grafos grandes
void cerrar_rama(void) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < contador_nodo; i++) {
            if (graph[i].estado == PENDIENTE) {
                for (int j = 0; j < graph[i].contador_padres; j++) {
                    int parent_idx = encontrar_id(graph[i].id_padre[j]);
                    if (parent_idx != -1 && graph[parent_idx].estado == FALLO) {
                        graph[i].estado = FALLO;
                        printf("[ABORTADO] Tarea %s abortada por fallo en dependencia %s.\n", graph[i].id, graph[i].id_padre[j]);
                        changed = 1;
                        break;
                    }
                }
            }
        }
    }
}

void notify_dependents(const char *COMPLETADO_id) {
    for (int i = 0; i < contador_nodo; i++) {
        if (graph[i].estado == PENDIENTE) {
            for (int j = 0; j < graph[i].contador_padres; j++) {
                if (strcmp(graph[i].id_padre[j], COMPLETADO_id) == 0) {
                    graph[i].dependencias--;
                }
            }
        }
    }
}

void imprimir_resumen(void) {
    int completadas = 0;
    int fallidas = 0;

    for (int i = 0; i < contador_nodo; i++) {
        if (graph[i].estado == COMPLETADO) {
            completadas++;
        } else if (graph[i].estado == FALLO) {
            fallidas++;
        }
    }

    printf("\nResumen: %d tareas procesadas (%d exitosas, %d canceladas).\n", 
           contador_nodo, completadas, fallidas);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s plan.txt K\n", argv[0]);
        return EXIT_FAILURE;
    }

    int K = atoi(argv[2]);//de texto a entero
    if (K <= 0) {
        fprintf(stderr, "El número K debe ser un entero positivo mayor a 0.\n");
        return EXIT_FAILURE;
    }

    srand(time(NULL));

    struct sigaction sa;                   //todo esto es cuando se presiona CTRL+C y que eso llame a la función
    sa.sa_handler = inspeccion_seremi;     //inspeccion_seremi, de manera que la ejecución se detiene
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    Leer_archivo(argv[1]);
    
    // Validación del grafo cargado
    if (!validar_grafo()) {
        fprintf(stderr, "Error: El archivo de plan contiene dependencias inválidas. Abortando.\n");
        return EXIT_FAILURE;
    }

    int proceso_activo = 0;
    int tarea_procesada = 0;

    while (tarea_procesada < contador_nodo) {
        int iniciacion = 0;

        for (int i = 0; i < contador_nodo; i++) {
            if (graph[i].estado == PENDIENTE && graph[i].dependencias == 0 && proceso_activo < K) {
                if (pipe(graph[i].pipe_fd) == -1) {
                    perror("Error al crear pipe");
                    exit(EXIT_FAILURE);
                }

                graph[i].estado = EJECUTANDOSE;
                proceso_activo++;
                iniciacion = 1;

                pid_t pid = fork();
                if (pid < 0) {
                    perror("Error en fork");
                    exit(EXIT_FAILURE);
                } else if (pid == 0) {
                    hacer_tarea(&graph[i]);
                } else {
                    graph[i].pid = pid;
                    close(graph[i].pipe_fd[1]); // cierra el extremo de escritura en el padre
                }
            }
        }

        // detección de bloqueos 
        if (proceso_activo == 0 && !iniciacion) {
            for (int i = 0; i < contador_nodo; i++) {
                if (graph[i].estado == PENDIENTE) {
                    graph[i].estado = FALLO;
                    tarea_procesada++;
                }
            }
            break;
        }

        if (proceso_activo > 0) {
            int estado;
            pid_t pid_terminado = waitpid(-1, &estado, 0); //lee al que ya termino

            if (pid_terminado > 0) {
                proceso_activo--;
                tarea_procesada++;

                for (int i = 0; i < contador_nodo; i++) {
                    if (graph[i].pid == pid_terminado) {
                        char buffer[128] = {0};
                        read(graph[i].pipe_fd[0], buffer, sizeof(buffer) - 1);
                        close(graph[i].pipe_fd[0]); 

                        if (WIFEXITED(estado) && WEXITSTATUS(estado) == EXIT_SUCCESS) {
                            graph[i].estado = COMPLETADO;
                            notify_dependents(graph[i].id);
                        } else {
                            graph[i].estado = FALLO;
                            printf("[ERROR] Tarea %s falló. Abortando su rama asociada...\n", graph[i].id);
                            cerrar_rama();
                        }
                        break;
                    }
                }
            }
        }
    }

    printf("\nEl#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

#define MAX_ID_LEN 64
#define MAX_NAME_LEN 64

typedef enum { PENDIENTE, EJECUTANDOSE, COMPLETADO, FALLO } TipoEstado;

typedef struct {
    char id[MAX_ID_LEN];
    char name[MAX_NAME_LEN];
    int duracion_ms;
    
    char **id_padre;
    int contador_padres;
    
    int dependencias;
    TipoEstado estado;
    pid_t pid;
    int pipe_fd[2];
} NodoTarea;

NodoTarea *graph = NULL;
int contador_nodo = 0;
int capacidad = 0;

// Busca el índice de una tarea en el grafo a partir de su ID alfanumérico
int encontrar_id(const char *id) {
    for (int i = 0; i < contador_nodo; i++) {
        if (strcmp(graph[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

void inspeccion_seremi(int sig) {
    (void)sig;
    printf("\n[SEREMI] Inspección Seremi. Se cancelan todas las actividades.\n");
    for (int i = 0; i < contador_nodo; i++) {
        if (graph[i].estado == EJECUTANDOSE && graph[i].pid > 0) {
            kill(graph[i].pid, SIGTERM);
        }
    }
    exit(EXIT_FAILURE);
}

int obtener_duracion_random(void) {
    return (rand() % 4901) + 100; // en esta parte es donde se le otorga 100 ms a 5000 ms de manera random, por si no se le asigna
}

// esto comprueba que todas las dependencias en el archivo existan en el grafo
int validar_grafo(void) {
    for (int i = 0; i < contador_nodo; i++) {
        for (int j = 0; j < graph[i].contador_padres; j++) {
            if (encontrar_id(graph[i].id_padre[j]) == -1) {
                fprintf(stderr, "[ERROR] La tarea '%s' depende de '%s', pero esa dependencia no existe o fallo.\n", 
                        graph[i].id, graph[i].id_padre[j]);
                return 0; // Grafo inválido
            }
        }
    }
    return 1;
}

void Leer_archivo(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error al abrir el archivo de plan");
        exit(EXIT_FAILURE);
    }

    capacidad = 100;
    graph = malloc(capacidad * sizeof(NodoTarea));

    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;

        if (contador_nodo >= capacidad) {
            capacidad *= 2;
            graph = realloc(graph, capacidad * sizeof(NodoTarea));
        }

        NodoTarea *node = &graph[contador_nodo];
        node->id_padre = NULL;
        node->contador_padres = 0;
        node->estado = PENDIENTE;
        node->pid = -1;

        line[strcspn(line, "\r\n")] = 0; //esto elimina los saltos de linea, para evitar errores

        char id_str[MAX_ID_LEN] = {0};
        char nombre_str[MAX_NAME_LEN] = {0};
        char duracion_str[32] = {0};
        char dependencias_str[512] = {0};

        int fields = sscanf(line, "%63[^:]:%63[^:]:%31[^:]:%511s", id_str, nombre_str, duracion_str, dependencias_str);
        //separa las cosas, el id, el nombre, duración y las dependencias

        if (fields < 2) continue;

        // Limpia espacios en blanco
        sscanf(id_str, " %63s", node->id);
        sscanf(nombre_str, " %63[^\n\r]", node->name);

        if (fields >= 3 && strlen(duracion_str) > 0 && atoi(duracion_str) > 0) {
            node->duracion_ms = atoi(duracion_str);
        } else {
            node->duracion_ms = obtener_duracion_random();
        }

        if (fields == 4 && strlen(dependencias_str) > 0) {
            int parent_cap = 5;
            node->id_padre = malloc(parent_cap * sizeof(char *));
            
            char *dep = strtok(dependencias_str, ","); //apunta al primer elemento en una lista de comas
            while (dep) {
                if (node->contador_padres >= parent_cap) {
                    parent_cap *= 2;
                    node->id_padre = realloc(node->id_padre, parent_cap * sizeof(char *));
                }
                
                // remueve espacios en las depencencias
                char clean_dep[MAX_ID_LEN] = {0};
                sscanf(dep, " %63s", clean_dep);
                
                node->id_padre[node->contador_padres] = strdup(clean_dep);
                node->contador_padres++;
                dep = strtok(NULL, ",");
            }
        }

        node->dependencias = node->contador_padres;
        contador_nodo++;
    }
    fclose(file);
}

void hacer_tarea(NodoTarea *node) {
    signal(SIGINT, SIG_DFL);

    close(node->pipe_fd[0]); // cierra el extremo de lectura en el hijo
    printf("[INICIO] Tarea %s (%s) - Duración: %d ms\n", node->id, node->name, node->duracion_ms);

    usleep(node->duracion_ms * 1000);

    char msg[128];
    snprintf(msg, sizeof(msg), "COMPLETADO:%s", node->id);
    
    // aca se transmiten los mensajes a través de Pipe
    if (write(node->pipe_fd[1], msg, strlen(msg)) == -1) {
        perror("Error escribiendo en pipe");
    }
    
    close(node->pipe_fd[1]);
    printf("[FIN] Tarea %s (%s) completada.\n", node->id, node->name);
    exit(EXIT_SUCCESS);
}

// Propagación iterativa para evitar overflow con grafos grandes
void cerrar_rama(void) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < contador_nodo; i++) {
            if (graph[i].estado == PENDIENTE) {
                for (int j = 0; j < graph[i].contador_padres; j++) {
                    int parent_idx = encontrar_id(graph[i].id_padre[j]);
                    if (parent_idx != -1 && graph[parent_idx].estado == FALLO) {
                        graph[i].estado = FALLO;
                        printf("[ABORTADO] Tarea %s abortada por fallo en dependencia %s.\n", graph[i].id, graph[i].id_padre[j]);
                        changed = 1;
                        break;
                    }
                }
            }
        }
    }
}

void notify_dependents(const char *COMPLETADO_id) {
    for (int i = 0; i < contador_nodo; i++) {
        if (graph[i].estado == PENDIENTE) {
            for (int j = 0; j < graph[i].contador_padres; j++) {
                if (strcmp(graph[i].id_padre[j], COMPLETADO_id) == 0) {
                    graph[i].dependencias--;
                }
            }
        }
    }
}

void imprimir_resumen(void) {
    int completadas = 0;
    int fallidas = 0;

    for (int i = 0; i < contador_nodo; i++) {
        if (graph[i].estado == COMPLETADO) {
            completadas++;
        } else if (graph[i].estado == FALLO) {
            fallidas++;
        }
    }

    printf("\nResumen: %d tareas procesadas (%d exitosas, %d canceladas).\n", 
           contador_nodo, completadas, fallidas);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s plan.txt K\n", argv[0]);
        return EXIT_FAILURE;
    }

    int K = atoi(argv[2]);//de texto a entero
    if (K <= 0) {
        fprintf(stderr, "El número K debe ser un entero positivo mayor a 0.\n");
        return EXIT_FAILURE;
    }

    srand(time(NULL));

    struct sigaction sa;                   //todo esto es cuando se presiona CTRL+C y que eso llame a la función
    sa.sa_handler = inspeccion_seremi;     //inspeccion_seremi, de manera que la ejecución se detiene
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    Leer_archivo(argv[1]);
    
    // Validación del grafo cargado
    if (!validar_grafo()) {
        fprintf(stderr, "Error: El archivo de plan contiene dependencias inválidas. Abortando.\n");
        return EXIT_FAILURE;
    }

    int proceso_activo = 0;
    int tarea_procesada = 0;

    while (tarea_procesada < contador_nodo) {
        int iniciacion = 0;

        for (int i = 0; i < contador_nodo; i++) {
            if (graph[i].estado == PENDIENTE && graph[i].dependencias == 0 && proceso_activo < K) {
                if (pipe(graph[i].pipe_fd) == -1) {
                    perror("Error al crear pipe");
                    exit(EXIT_FAILURE);
                }

                graph[i].estado = EJECUTANDOSE;
                proceso_activo++;
                iniciacion = 1;

                pid_t pid = fork();
                if (pid < 0) {
                    perror("Error en fork");
                    exit(EXIT_FAILURE);
                } else if (pid == 0) {
                    hacer_tarea(&graph[i]);
                } else {
                    graph[i].pid = pid;
                    close(graph[i].pipe_fd[1]); // cierra el extremo de escritura en el padre
                }
            }
        }

        // detección de bloqueos 
        if (proceso_activo == 0 && !iniciacion) {
            for (int i = 0; i < contador_nodo; i++) {
                if (graph[i].estado == PENDIENTE) {
                    graph[i].estado = FALLO;
                    tarea_procesada++;
                }
            }
            break;
        }

        if (proceso_activo > 0) {
            int estado;
            pid_t pid_terminado = waitpid(-1, &estado, 0); //lee al que ya termino

            if (pid_terminado > 0) {
                proceso_activo--;
                tarea_procesada++;

                for (int i = 0; i < contador_nodo; i++) {
                    if (graph[i].pid == pid_terminado) {
                        char buffer[128] = {0};
                        read(graph[i].pipe_fd[0], buffer, sizeof(buffer) - 1);
                        close(graph[i].pipe_fd[0]); 

                        if (WIFEXITED(estado) && WEXITSTATUS(estado) == EXIT_SUCCESS) {
                            graph[i].estado = COMPLETADO;
                            notify_dependents(graph[i].id);
                        } else {
                            graph[i].estado = FALLO;
                            printf("[ERROR] Tarea %s falló. Abortando su rama asociada...\n", graph[i].id);
                            cerrar_rama();
                        }
                        break;
                    }
                }
            }
        }
    }

    printf("\nEl plan ha sido completado\n");
    
    imprimir_resumen();

    // Limpieza de memoria asignada dinámicamente#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

#define MAX_ID_LEN 64
#define MAX_NAME_LEN 64

typedef enum { PENDIENTE, EJECUTANDOSE, COMPLETADO, FALLO } TipoEstado;

typedef struct {
    char id[MAX_ID_LEN];
    char name[MAX_NAME_LEN];
    int duracion_ms;
    
    char **id_padre;
    int contador_padres;
    
    int dependencias;
    TipoEstado estado;
    pid_t pid;
    int pipe_fd[2];
} NodoTarea;

NodoTarea *graph = NULL;
int contador_nodo = 0;
int capacidad = 0;

// Busca el índice de una tarea en el grafo a partir de su ID alfanumérico
int encontrar_id(const char *id) {
    for (int i = 0; i < contador_nodo; i++) {
        if (strcmp(graph[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

void inspeccion_seremi(int sig) {
    (void)sig;
    printf("\n[SEREMI] Inspección Seremi. Se cancelan todas las actividades.\n");
    for (int i = 0; i < contador_nodo; i++) {
        if (graph[i].estado == EJECUTANDOSE && graph[i].pid > 0) {
            kill(graph[i].pid, SIGTERM);
        }
    }
    exit(EXIT_FAILURE);
}

int obtener_duracion_random(void) {
    return (rand() % 4901) + 100; // en esta parte es donde se le otorga 100 ms a 5000 ms de manera random, por si no se le asigna
}

// esto comprueba que todas las dependencias en el archivo existan en el grafo
int validar_grafo(void) {
    for (int i = 0; i < contador_nodo; i++) {
        for (int j = 0; j < graph[i].contador_padres; j++) {
            if (encontrar_id(graph[i].id_padre[j]) == -1) {
                fprintf(stderr, "[ERROR] La tarea '%s' depende de '%s', pero esa dependencia no existe o fallo.\n", 
                        graph[i].id, graph[i].id_padre[j]);
                return 0; // Grafo inválido
            }
        }
    }
    return 1;
}

void Leer_archivo(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error al abrir el archivo de plan");
        exit(EXIT_FAILURE);
    }

    capacidad = 100;
    graph = malloc(capacidad * sizeof(NodoTarea));

    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;

        if (contador_nodo >= capacidad) {
            capacidad *= 2;
            graph = realloc(graph, capacidad * sizeof(NodoTarea));
        }

        NodoTarea *node = &graph[contador_nodo];
        node->id_padre = NULL;
        node->contador_padres = 0;
        node->estado = PENDIENTE;
        node->pid = -1;

        line[strcspn(line, "\r\n")] = 0; //esto elimina los saltos de linea, para evitar errores

        char id_str[MAX_ID_LEN] = {0};
        char nombre_str[MAX_NAME_LEN] = {0};
        char duracion_str[32] = {0};
        char dependencias_str[512] = {0};

        int fields = sscanf(line, "%63[^:]:%63[^:]:%31[^:]:%511s", id_str, nombre_str, duracion_str, dependencias_str);
        //separa las cosas, el id, el nombre, duración y las dependencias

        if (fields < 2) continue;

        // Limpia espacios en blanco
        sscanf(id_str, " %63s", node->id);
        sscanf(nombre_str, " %63[^\n\r]", node->name);

        if (fields >= 3 && strlen(duracion_str) > 0 && atoi(duracion_str) > 0) {
            node->duracion_ms = atoi(duracion_str);
        } else {
            node->duracion_ms = obtener_duracion_random();
        }

        if (fields == 4 && strlen(dependencias_str) > 0) {
            int parent_cap = 5;
            node->id_padre = malloc(parent_cap * sizeof(char *));
            
            char *dep = strtok(dependencias_str, ","); //apunta al primer elemento en una lista de comas
            while (dep) {
                if (node->contador_padres >= parent_cap) {
                    parent_cap *= 2;
                    node->id_padre = realloc(node->id_padre, parent_cap * sizeof(char *));
                }
                
                // remueve espacios en las depencencias
                char clean_dep[MAX_ID_LEN] = {0};
                sscanf(dep, " %63s", clean_dep);
                
                node->id_padre[node->contador_padres] = strdup(clean_dep);
                node->contador_padres++;
                dep = strtok(NULL, ",");
            }
        }

        node->dependencias = node->contador_padres;
        contador_nodo++;
    }
    fclose(file);
}

void hacer_tarea(NodoTarea *node) {
    signal(SIGINT, SIG_DFL);

    close(node->pipe_fd[0]); // cierra el extremo de lectura en el hijo
    printf("[INICIO] Tarea %s (%s) - Duración: %d ms\n", node->id, node->name, node->duracion_ms);

    usleep(node->duracion_ms * 1000);

    char msg[128];
    snprintf(msg, sizeof(msg), "COMPLETADO:%s", node->id);
    
    // aca se transmiten los mensajes a través de Pipe
    if (write(node->pipe_fd[1], msg, strlen(msg)) == -1) {
        perror("Error escribiendo en pipe");
    }
    
    close(node->pipe_fd[1]);
    printf("[FIN] Tarea %s (%s) completada.\n", node->id, node->name);
    exit(EXIT_SUCCESS);
}

// Propagación iterativa para evitar overflow con grafos grandes
void cerrar_rama(void) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < contador_nodo; i++) {
            if (graph[i].estado == PENDIENTE) {
                for (int j = 0; j < graph[i].contador_padres; j++) {
                    int parent_idx = encontrar_id(graph[i].id_padre[j]);
                    if (parent_idx != -1 && graph[parent_idx].estado == FALLO) {
                        graph[i].estado = FALLO;
                        printf("[ABORTADO] Tarea %s abortada por fallo en dependencia %s.\n", graph[i].id, graph[i].id_padre[j]);
                        changed = 1;
                        break;
                    }
                }
            }
        }
    }
}

void notify_dependents(const char *COMPLETADO_id) {
    for (int i = 0; i < contador_nodo; i++) {
        if (graph[i].estado == PENDIENTE) {
            for (int j = 0; j < graph[i].contador_padres; j++) {
                if (strcmp(graph[i].id_padre[j], COMPLETADO_id) == 0) {
                    graph[i].dependencias--;
                }
            }
        }
    }
}

void imprimir_resumen(void) {
    int completadas = 0;
    int fallidas = 0;

    for (int i = 0; i < contador_nodo; i++) {
        if (graph[i].estado == COMPLETADO) {
            completadas++;
        } else if (graph[i].estado == FALLO) {
            fallidas++;
        }
    }

    printf("\nResumen: %d tareas procesadas (%d exitosas, %d canceladas).\n", 
           contador_nodo, completadas, fallidas);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s plan.txt K\n", argv[0]);
        return EXIT_FAILURE;
    }

    int K = atoi(argv[2]);//de texto a entero
    if (K <= 0) {
        fprintf(stderr, "El número K debe ser un entero positivo mayor a 0.\n");
        return EXIT_FAILURE;
    }

    srand(time(NULL));

    struct sigaction sa;                   //todo esto es cuando se presiona CTRL+C y que eso llame a la función
    sa.sa_handler = inspeccion_seremi;     //inspeccion_seremi, de manera que la ejecución se detiene
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    Leer_archivo(argv[1]);
    
    // valida al grafo cargado
    if (!validar_grafo()) {
        fprintf(stderr, "Error: El archivo de plan contiene dependencias inválidas. Abortando.\n");
        return EXIT_FAILURE;
    }

    int proceso_activo = 0;
    int tarea_procesada = 0;

    while (tarea_procesada < contador_nodo) {
        int iniciacion = 0;

        for (int i = 0; i < contador_nodo; i++) {
            if (graph[i].estado == PENDIENTE && graph[i].dependencias == 0 && proceso_activo < K) {
                if (pipe(graph[i].pipe_fd) == -1) {
                    perror("Error al crear pipe");
                    exit(EXIT_FAILURE);
                }

                graph[i].estado = EJECUTANDOSE;
                proceso_activo++;
                iniciacion = 1;

                pid_t pid = fork();
                if (pid < 0) {
                    perror("Error en fork");
                    exit(EXIT_FAILURE);
                } else if (pid == 0) {
                    hacer_tarea(&graph[i]);
                } else {
                    graph[i].pid = pid;
                    close(graph[i].pipe_fd[1]); // cierra el extremo de escritura en el padre
                }
            }
        }

        // detección de bloqueos 
        if (proceso_activo == 0 && !iniciacion) {
            for (int i = 0; i < contador_nodo; i++) {
                if (graph[i].estado == PENDIENTE) {
                    graph[i].estado = FALLO;
                    tarea_procesada++;
                }
            }
            break;
        }

        if (proceso_activo > 0) {
            int estado;
            pid_t pid_terminado = waitpid(-1, &estado, 0); //lee al que ya termino

            if (pid_terminado > 0) {
                proceso_activo--;
                tarea_procesada++;

                for (int i = 0; i < contador_nodo; i++) {
                    if (graph[i].pid == pid_terminado) {
                        char buffer[128] = {0};
                        read(graph[i].pipe_fd[0], buffer, sizeof(buffer) - 1);
                        close(graph[i].pipe_fd[0]); 

                        if (WIFEXITED(estado) && WEXITSTATUS(estado) == EXIT_SUCCESS) {
                            graph[i].estado = COMPLETADO;
                            notify_dependents(graph[i].id);
                        } else {
                            graph[i].estado = FALLO;
                            printf("[ERROR] Tarea %s falló. Abortando su rama asociada...\n", graph[i].id);
                            cerrar_rama();
                        }
                        break;
                    }
                }
            }
        }
    }

    printf("\nEl plan ha sido completado\n");
    
    imprimir_resumen();
    
    for (int i = 0; i < contador_nodo; i++) {
        for (int j = 0; j < graph[i].contador_padres; j++) {
            free(graph[i].id_padre[j]);
        }
        free(graph[i].id_padre);
    }
    free(graph);

    return EXIT_SUCCESS;
}
    for (int i = 0; i < contador_nodo; i++) {
        for (int j = 0; j < graph[i].contador_padres; j++) {
            free(graph[i].id_padre[j]);
        }
        free(graph[i].id_padre);
    }
    free(graph);

    return EXIT_SUCCESS;
} plan ha sido completado\n");
    
    imprimir_resumen();
    
    for (int i = 0; i < contador_nodo; i++) {
        for (int j = 0; j < graph[i].contador_padres; j++) {
            free(graph[i].id_padre[j]);
        }
        free(graph[i].id_padre);
    }
    free(graph);

    return EXIT_SUCCESS;
}n del grafo cargado
    if (!validar_grafo()) {
        fprintf(stderr, "Error: El archivo de plan contiene dependencias inválidas. Abortando.\n");
        return EXIT_FAILURE;
    }

    int proceso_activo = 0;
    int tarea_procesada = 0;

    while (tarea_procesada < contador_nodo) {
        int iniciacion = 0;

        for (int i = 0; i < contador_nodo; i++) {
            if (graph[i].estado == PENDIENTE && graph[i].dependencias == 0 && proceso_activo < K) {
                if (pipe(graph[i].pipe_fd) == -1) {
                    perror("Error al crear pipe");
                    exit(EXIT_FAILURE);
                }

                graph[i].estado = EJECUTANDOSE;
                proceso_activo++;
                iniciacion = 1;

                pid_t pid = fork();
                if (pid < 0) {
                    perror("Error en fork");
                    exit(EXIT_FAILURE);
                } else if (pid == 0) {
                    hacer_tarea(&graph[i]);
                } else {
                    graph[i].pid = pid;
                    close(graph[i].pipe_fd[1]); // cierra el extremo de escritura en el padre
                }
            }
        }

        // detección de bloqueos 
        if (proceso_activo == 0 && !iniciacion) {
            for (int i = 0; i < contador_nodo; i++) {
                if (graph[i].estado == PENDIENTE) {
                    graph[i].estado = FALLO;
                    tarea_procesada++;
                }
            }
            break;
        }

        if (proceso_activo > 0) {
            int estado;
            pid_t pid_terminado = waitpid(-1, &estado, 0); //lee al que ya termino

            if (pid_terminado > 0) {
                proceso_activo--;
                tarea_procesada++;

                for (int i = 0; i < contador_nodo; i++) {
                    if (graph[i].pid == pid_terminado) {
                        char buffer[128] = {0};
                        read(graph[i].pipe_fd[0], buffer, sizeof(buffer) - 1);
                        close(graph[i].pipe_fd[0]); 

                        if (WIFEXITED(estado) && WEXITSTATUS(estado) == EXIT_SUCCESS) {
                            graph[i].estado = COMPLETADO;
                            notify_dependents(graph[i].id);
                        } else {
                            graph[i].estado = FALLO;
                            printf("[ERROR] Tarea %s falló. Abortando su rama asociada...\n", graph[i].id);
                            cerrar_rama();
                        }
                        break;
                    }
                }
            }
        }
    }

    printf("\nEl plan ha sido completado\n");
    
    imprimir_resumen();

    // limpieza de memoria
    for (int i = 0; i < contador_nodo; i++) {
        for (int j = 0; j < graph[i].contador_padres; j++) {
            free(graph[i].id_padre[j]);
        }
        free(graph[i].id_padre);
    }
    free(graph);

    return EXIT_SUCCESS;
}
