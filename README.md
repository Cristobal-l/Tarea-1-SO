# Planificador Dieciochero

Simulador de actividades de la ramada del señor Loyola, modeladas como un DAG.
Ejecuta cada actividad en su propio proceso, respetando dependencias y un
límite de concurrencia `K`.

## Compilación

```
make
```

Compila con `g++ -Wall -Wextra -std=c++17 -lpthread` (ver `Makefile`).

## Uso

```
./planificador plan.txt K
```

- `plan.txt`: archivo con las actividades, formato `ID : Nombre : tiempo_ms : dep1, dep2, ...`
  (tiempo_ms puede omitirse; en ese caso se asigna aleatoriamente entre 100 y 5000 ms).
- `K`: número máximo de procesos corriendo simultáneamente.

Ejemplo:

```
make
./planificador plan.txt 2
```

Para interrumpir la ejecución (simula la inspección de la Seremi) usar `Ctrl+C`:
aborta todas las actividades en curso.

## Estructura del código

- `actividad.hpp`: struct `Actividad`, un nodo del DAG (id, nombre, tiempo, sus
  dependencias, sus dependientes, y el estado de ejecución).
- `parser.hpp` / `parser.cpp`: lee `plan.txt` y arma el grafo completo:
  primera pasada crea cada nodo, segunda pasada llena la lista inversa
  `dependientes` y calcula `grado_entrada` de cada uno.
- `planificador.hpp` / `planificador.cpp`: núcleo del programa.
  - `ejecutar_plan`: loop principal. Mantiene una cola de actividades listas
    (grado de entrada 0) y lanza procesos con `fork()` mientras haya cupo
    (`running < K`). Usa `wait()` (bloqueante) para esperar a que cualquier
    hijo termine — sin busy-waiting.
  - `ejecutar_actividad`: código que corre el proceso hijo — simula el
    trabajo (`usleep`), puede fallar con una probabilidad configurable, y
    escribe su mensaje de resultado a un pipe.
  - `abortar_rama`: cuando una actividad falla, marca como abortados (de
    forma iterativa, con una cola) a todos sus descendientes, sin afectar
    el resto del DAG.
  - Manejo de `SIGINT`: un manejador mínimo (solo levanta una bandera
    `sig_atomic_t`, que es lo único seguro de tocar dentro de un signal
    handler) que el loop principal revisa para matar con `SIGTERM` a todos
    los procesos activos y esperar a que terminen antes de salir.
- `main.cpp`: parsea los argumentos de línea de comandos y llama a las
  funciones anteriores.

## Decisiones de diseño

**Paso de mensajes vía pipes.** Cada actividad crea un pipe antes de
forkearse. El hijo escribe su mensaje de resultado al pipe y termina; el
padre lo lee justo después de reapearlo con `wait()` y lo guarda en memoria
(`Actividad::mensaje`). Cuando el padre lanza a un dependiente, este ya
"recibe" los mensajes de sus dependencias porque `fork()` copia toda la
memoria del padre en ese momento — no hace falta un pipe explícito por cada
arista del grafo. Se eligió este diseño (un pipe por nodo, con el padre como
intermediario) en vez de un pipe por arista porque una actividad puede tener
varios dependientes, y esto evita crear un número de pipes proporcional a
las aristas del DAG, manteniendo el código más simple sin perder aislamiento
entre procesos (los pipes siguen siendo el único canal de IPC real usado).

**Aislamiento de errores sin hilos.** Cada actividad corre en un proceso
`fork()`-eado, con memoria completamente separada del padre y de sus
hermanos. Si una actividad falla (código de salida != 0), el padre lo
detecta vía `WEXITSTATUS` y aborta recursivamente (de forma iterativa, para
soportar cargas grandes sin riesgo de overflow de pila) solo la rama que
dependía de ella, sin afectar al resto del plan.

**Sin busy-waiting ni race conditions.** El control de concurrencia usa
`wait()`, que bloquea al proceso padre hasta que cualquier hijo termina, en
vez de hacer polling. No hay condiciones de carrera porque no hay memoria
compartida entre procesos: cada uno tiene su copia privada tras el `fork()`,
y el único proceso que toca la estructura del grafo (`std::map<int,
Actividad>`) es el padre.

**Manejo de SIGINT.** El manejador de señal solo modifica una variable
`volatile sig_atomic_t` (lo único garantizado seguro dentro de un signal
handler). Toda la lógica real de limpieza —mandar `SIGTERM` a los procesos
activos y esperarlos con `waitpid`— se hace en el loop principal, no dentro
del handler.

**Buffering de `std::cout` y `fork()`.** `std::cout` se bufferea por
completo cuando la salida no es una terminal (por ejemplo, al redirigir a un
archivo con `>`, como en las pruebas de carga). Si el proceso padre tiene
contenido pendiente sin escribir al momento de un `fork()`, el hijo hereda
una copia de ese buffer y puede terminar flusheándolo por su cuenta,
duplicando líneas cuando el padre lo flushea también más tarde. Se usa
`std::cout.setf(std::ios_base::unitbuf)` para forzar un flush automático
después de cada inserción, evitando que quede contenido pendiente en el
buffer al momento de cualquier `fork()`.

## Pruebas

- `plan.txt` de ejemplo del enunciado (6 actividades) con distintos valores
  de `K`.
- Carga de 10000 actividades generadas aleatoriamente (dependencias válidas,
  DAG acíclico) para validar la sección 3.3 del enunciado — sin duplicar ni
  perder actividades (verificado: completadas + fallidas + abortadas =
  total), y sin dejar procesos zombis al finalizar.
- `Ctrl+C` durante una ejecución en curso, para validar el aislamiento vía
  `SIGINT`.
