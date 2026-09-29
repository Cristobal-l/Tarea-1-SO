#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <queue>
#include <set>
#include <random>
#include <cstring>
#include <cerrno>

#include <unistd.h>
#include <sys/wait.h>
#include <csignal>

// DAG
struct Actividad {
    int id = 0;
    std::string nombre;
    int tiempo_ms = 0;

    std::vector<int> dependencias;   // IDs de quienes deben terminar antes
    std::vector<int> dependientes;   // IDs de quienes dependen de esta actividad

    int grado_entrada = 0;
    bool abortada = false;
    bool completada = false;

    std::string mensaje;
    int pipe_lectura = -1;
};

static std::string trim(const std::string& s) {
    size_t inicio = s.find_first_not_of(" \t\r\n");
    if (inicio == std::string::npos) return "";
    size_t fin = s.find_last_not_of(" \t\r\n");
    return s.substr(inicio, fin - inicio + 1);
}


// Parseo
std::map<int, Actividad> parsear_plan(const std::string& ruta_archivo) {
    std::ifstream archivo(ruta_archivo);
    if (!archivo.is_open()) {
        throw std::runtime_error("No se pudo abrir el archivo: " + ruta_archivo);
    }

    std::map<int, Actividad> actividades;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist_tiempo(100, 5000);

    std::string linea;
    while (std::getline(archivo, linea)) {
        linea = trim(linea);
        if (linea.empty()) continue;

        // Formato: ID : Nombre : tiempo_ms : dep1, dep2, ...
        std::stringstream ss(linea);
        std::string campo_id, campo_nombre, campo_tiempo, campo_deps;

        std::getline(ss, campo_id, ':');
        std::getline(ss, campo_nombre, ':');
        std::getline(ss, campo_tiempo, ':');
        std::getline(ss, campo_deps);

        Actividad act;
        act.id = std::stoi(trim(campo_id));
        act.nombre = trim(campo_nombre);

        std::string tiempo_trim = trim(campo_tiempo);
        act.tiempo_ms = tiempo_trim.empty() ? dist_tiempo(gen) : std::stoi(tiempo_trim);

        std::stringstream deps_ss(campo_deps);
        std::string dep;
        while (std::getline(deps_ss, dep, ',')) {
            std::string dep_trim = trim(dep);
            if (!dep_trim.empty()) act.dependencias.push_back(std::stoi(dep_trim));
        }

        act.grado_entrada = static_cast<int>(act.dependencias.size());
        actividades[act.id] = act;
    }

    // Revisar dependencias
    for (auto& [id, act] : actividades) {
        for (int dep_id : act.dependencias) {
            if (actividades.find(dep_id) == actividades.end()) {
                throw std::runtime_error("La actividad " + std::to_string(id) +
                    " depende de un ID inexistente: " + std::to_string(dep_id));
            }
            actividades[dep_id].dependientes.push_back(id);
        }
    }

    return actividades;
}

// SIGINT Inspección de la Seremi
static volatile sig_atomic_t g_sigint_recibido = 0;

static void manejador_sigint(int) {
    g_sigint_recibido = 1;
}

// Lo que corre cada actividad
static void ejecutar_actividad(const Actividad& act,
                                const std::vector<std::string>& mensajes_entrada,
                                int fd_escritura) {
    for (const auto& msg : mensajes_entrada) {
        std::cout << "[" << act.nombre << "] insumo recibido: " << msg << "\n";
    }

    usleep(static_cast<useconds_t>(act.tiempo_ms) * 1000);

    // Simulación de falla interna
    std::random_device rd;
    std::mt19937 gen(rd() ^ static_cast<unsigned>(getpid()));
    std::uniform_real_distribution<double> prob(0.0, 1.0);
    const double PROBABILIDAD_FALLA = 0.05;

    if (prob(gen) < PROBABILIDAD_FALLA) {
        std::string msg_error = "ERROR interno en " + act.nombre;
        write(fd_escritura, msg_error.c_str(), msg_error.size());
        close(fd_escritura);
        _exit(1);
    }

    std::string mensaje_salida =
        act.nombre + " completado en " + std::to_string(act.tiempo_ms) + "ms";
    write(fd_escritura, mensaje_salida.c_str(), mensaje_salida.size());
    close(fd_escritura);
    _exit(0);
}

// Aborta toda la rama que dependía de una actividad fallida
static void abortar_rama(int id_inicial, std::map<int, Actividad>& actividades, int& restantes) {
    std::queue<int> pendientes;
    pendientes.push(id_inicial);

    while (!pendientes.empty()) {
        int id = pendientes.front();
        pendientes.pop();

        for (int dep_id : actividades[id].dependientes) {
            Actividad& dep = actividades[dep_id];
            if (!dep.abortada) {
                dep.abortada = true;
                restantes--;
                std::cout << "[abortado] " << dep.nombre
                          << " (dependía de " << actividades[id].nombre << ")\n";
                pendientes.push(dep_id);
            }
        }
    }
}

// forks y waits con límite K, pipes de mensajes, aislamiento de fallos y manejo de SIGINT
void ejecutar_plan(std::map<int, Actividad>& actividades, int K) {
    struct sigaction sa {};
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    // Evita que el fork duplique contenido que el padre todavía no haya flusheado
    std::cout.setf(std::ios_base::unitbuf);

    std::queue<int> listos;
    int restantes = static_cast<int>(actividades.size());

    for (auto& [id, act] : actividades) {
        if (act.grado_entrada == 0) listos.push(id);
    }

    int running = 0;
    std::map<pid_t, int> pid_a_actividad;
    std::set<pid_t> pids_activos;

    while (restantes > 0) {
        if (g_sigint_recibido) {
            std::cout << "\n[SIGINT] Inspección de la Seremi: abortando todas las "
                         "actividades...\n";
            for (pid_t pid : pids_activos) kill(pid, SIGTERM);
            for (pid_t pid : pids_activos) {
                int status;
                waitpid(pid, &status, 0);
            }
            return;
        }

        while (running < K && !listos.empty()) {
            int id = listos.front();
            listos.pop();
            Actividad& act = actividades[id];

            // Los mensajes de entrada ya están en memoria porque las dependencias de "act" ya terminaron y el fork() copia esa memoria al hijo, así que no hace falta un pipe
            std::vector<std::string> entradas;
            for (int dep_id : act.dependencias) entradas.push_back(actividades[dep_id].mensaje);

            int fd[2];
            if (pipe(fd) == -1) {
                std::cerr << "Error creando pipe para " << act.nombre << "\n";
                continue;
            }

            pid_t pid = fork();
            if (pid < 0) {
                std::cerr << "Error en fork() para " << act.nombre << "\n";
                close(fd[0]);
                close(fd[1]);
                continue;
            }

            if (pid == 0) {
                close(fd[0]);
                ejecutar_actividad(act, entradas, fd[1]);
            }

            close(fd[1]);
            act.pipe_lectura = fd[0];
            pid_a_actividad[pid] = id;
            pids_activos.insert(pid);
            running++;
        }

     

        running--;
        pids_activos.erase(hijo);
        int id = pid_a_actividad[hijo];
        Actividad& act = actividades[id];
        restantes--;

        char buffer[256];
        ssize_t n = read(act.pipe_lectura, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            act.mensaje = std::string(buffer);
        }
        close(act.pipe_lectura);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            act.completada = true;
            std::cout << "[ok] " << act.mensaje << "\n";
            for (int dep_id : act.dependientes) {
                Actividad& dependiente = actividades[dep_id];
                if (dependiente.abortada) continue;
                dependiente.grado_entrada--;
                if (dependiente.grado_entrada == 0) listos.push(dep_id);
            }
        } else {
            std::cout << "[falla] " << act.nombre << " terminó con error\n";
            abortar_rama(id, actividades, restantes);
        }
    }

    std::cout << "Planificación finalizada.\n";
}


int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Uso: ./planificador plan.txt K\n";
        return 1;
    }

    std::string ruta_archivo = argv[1];
    int K = std::stoi(argv[2]);

    if (K <= 0) {
        std::cerr << "K debe ser un entero positivo.\n";
        return 1;
    }

    try {
        auto actividades = parsear_plan(ruta_archivo);
        ejecutar_plan(actividades, K);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
