//****************************************************************************
//                         REDES Y SISTEMAS DISTRIBUIDOS
//
//                     2º de grado de Ingeniería Informática
//
//              This class processes an FTP transaction.
//
//****************************************************************************

#include "ClientConnection.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <langinfo.h>
#include <locale.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wait.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "FTPServer.h"
#include "common.h"

ClientConnection::ClientConnection(int s) {
  int sock = (int)(s);

  char buffer[MAX_BUFF];

  control_socket = s;
  // Check the Linux man pages to know what fdopen does.
  fd = fdopen(s, "a+");
  if (fd == NULL) {
    std::cout << "Connection closed" << std::endl;

    fclose(fd);
    close(control_socket);
    ok = false;
    return;
  }

  ok = true;
  data_socket = -1;
  parar = false;
};

ClientConnection::~ClientConnection() {
  fclose(fd);
  close(control_socket);
}

int connect_TCP(uint32_t address, uint16_t port) {
  struct sockaddr_in sin;
  struct hostent *hent;
  int s;

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);

  memcpy(&sin.sin_addr, &address, sizeof(address));

  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) errexit("No se puede crear el socket: %s\n", strerror(errno));

  if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) < 0)
    errexit("No se puede conectar con: %s\n", strerror(errno));
  return s;
}

void ClientConnection::stop() {
  close(data_socket);
  close(control_socket);
  parar = true;
}

#define COMMAND(cmd) strcmp(command, cmd) == 0

// This method processes the requests.
// Here you should implement the actions related to the FTP commands.
// See the example for the USER command.
// If you think that you have to add other commands feel free to do so. You
// are allowed to add auxiliary methods if necessary.

void ClientConnection::WaitForRequests() {
  if (!ok) {
    return;
  }

  fprintf(fd, "220 Service ready\n");

  while (!parar) {
    fscanf(fd, "%s", command);
    if (COMMAND("USER")) {
      fscanf(fd, "%s", arg);
      fprintf(fd, "331 User name ok, need password\n");
      fflush(fd);
    } else if (COMMAND("PWD")) {
      fprintf(fd, "257 \"/\" is the current directory\n");
    } else if (COMMAND("PASS")) {
      fscanf(fd, "%s", arg);
      if (strcmp(arg, "1234") == 0) {
        fprintf(fd, "230 User logged in\n");
        fflush(fd);
      } else {
        fprintf(fd, "530 Not logged in.\n");
        fflush(fd);
        parar = true;
      }

    } else if (COMMAND("PORT")) {
      int h1, h2, h3, h4, p1, p2;
      fscanf(fd, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2);
      printf("IP: %d.%d.%d.%d\n", h1, h2, h3, h4);
      uint32_t address = (h4 << 24) | (h3 << 16) | (h2 << 8) | h1;
      uint16_t port = (p1 << 8) | p2;
      data_socket = connect_TCP(address, port);
      fprintf(fd, "200 Command okey.\n");
      fflush(fd);
    } else if (COMMAND("PASV")) {
      int s = define_socket_TCP(0);
      struct sockaddr_in sin;
      socklen_t len = sizeof(sin);
      getsockname(s, (struct sockaddr *)&sin, &len);
      int port = sin.sin_port;
      int port1 = port / 256;
      int port2 = port % 256;
      fprintf(fd, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)\n", 127, 0, 0,
              1, port2, port1);
      fflush(fd);
      data_socket = accept(s, (struct sockaddr *)&sin, &len);
    } else if (COMMAND("STOR")) {
      //Es necesario añadir un fichero en el servidor para poder subirlo, si se sube con el mismo nombre del fichero este se borra.
      fscanf(fd, "%s", arg);
      fprintf(fd, "150 File status okay; about to open data connection.\n");
      fflush(fd);
      FILE *fichero = fopen(arg, "wb");
      if (fichero == NULL) {
        fprintf(fd, "550 File not found.\n");
        fflush(fd);
        close(data_socket);
        break;
      }
      char *buffer[MAX_BUFF];
      while (1) {
        // Recibir del socket
        int leidos = recv(data_socket, buffer, MAX_BUFF, 0);
        // Escribir en el fichero
        fwrite(buffer, 1, leidos, fichero);
        if (leidos != MAX_BUFF) {
          break;
        }
      }
      fprintf(fd, "226 File received.\n");
      fflush(fd);
      fclose(fichero);
      close(data_socket);
      fflush(fd);
    } else if (COMMAND("RETR")) {
      fscanf(fd, "%s", arg);
      // Abrir el fichero (modo lectura, binario)
      FILE *fichero = fopen(arg, "rb");
      if (fichero == NULL) {
        fprintf(fd, "550 File not found.\n");
        fflush(fd);
        close(data_socket);
        continue;
      } else {
        fprintf(fd, "150 File status okay; about to open data connection.\n");
        fflush(fd);
        char *buffer[MAX_BUFF];
        while (1) {
          // Leer del fichero
          int leidos = fread(buffer, 1, MAX_BUFF, fichero);
          // Enviar al socket
          send(data_socket, buffer, leidos, 0);
          if (leidos != MAX_BUFF) {
            break;
          }
        }
        fclose(fichero);
        fprintf(fd, "226 File sent.\n");
      }
      // cerrar data socket
      close(data_socket);
    } else if (COMMAND("LIST")) {
      fprintf(fd, "150 Here comes the directory listing.\n");
      fflush(fd);
      // Crear un pipe
      int pipefd[2];
      pipe(pipefd);
      // Crear un proceso hijo
      pid_t pid = fork();
      if (pid == 0) {
        // Redirigir la salida estándar al pipe
        dup2(pipefd[1], 1);
        // Ejecutar el comando ls
        execlp("ls", "ls", "-l", NULL);
      } else {
        // Cerrar el descriptor de escritura del pipe
        close(pipefd[1]);
        // Leer del pipe y enviar al socket
        char buffer[MAX_BUFF];
        while (1) {
          int leidos = read(pipefd[0], buffer, MAX_BUFF);
          if (leidos <= 0) {
            break;
          }
          send(data_socket, buffer, leidos, 0);
        }
        // Cerrar el descriptor de lectura del pipe
        close(pipefd[0]);
        // Esperar al hijo
        wait(NULL);
        // Cerrar el socket de datos
        close(data_socket);
        // Enviar mensaje de finalización
        fprintf(fd, "226 Directory send OK.\n");
      }
    } else if (COMMAND("SYST")) {
      fprintf(fd, "215 UNIX Type: L8.\n");
    }

    else if (COMMAND("TYPE")) {
      fscanf(fd, "%s", arg);
      fprintf(fd, "200 OK\n");
    }

    else if (COMMAND("QUIT")) {
      fprintf(fd,
              "221 Service closing control connection. Logged out if "
              "appropriate.\n");
      close(data_socket);
      parar = true;
      break;
    }

    else {
      fprintf(fd, "502 Command not implemented.\n");
      fflush(fd);
      printf("Comando : %s %s\n", command, arg);
      printf("Error interno del servidor\n");
    }
  }

  fclose(fd);

  return;
};
