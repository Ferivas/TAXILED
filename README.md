# TAXILED
Driver para matriz 5x24 pixels basado en ATmega8
---

La matriz se conecta de manera que se multiplexan 8 lineas conectados a los anodos de los leds con switches de voltaje basados en un MOSFET P (FDN302). Para manejar los catodos de los leds se utiliza un driver de corriente constante de 16 salidas (A6282). Los leds se conectan agrupando un driver de columna (salida C0 del MOSFET P) con formando 5 columnas que tienen conectados todos sus anodos a este driver de voltaje y los catodos de los leds a cada una de las salidas del driver de corriente (F0,F1,F2 hasta F14), luego se tiene una nueva salida de otro driver de columna (salida C1 del MOSFET P) formando 5 columnas con los anodos de los leds conectados a C1 y los cátodos de los leds conectados a las salidas del driver de corriente (F0,F1,F2 hasta F14). Esto se repite para los restantes drivers de columna (C0 hasta C7).

## Pines utilizados

### Drivers de voltaje de columnas
- C0=PC0
- C1=PC1
- C2=PC2
- C3=PC3
- C4=PD4
- C5=PD5
- C6=PD6
- C7=PD7

### Driver de corriente constante de 16 salidas 
- SDI=PD3
- CLK=PB5
- LE=PB0
- OE=PC4

### Led de Señalizacion
Hay un led conectado a PB2 activado por bajo

### Pines de programacion
Existe un puerto de programacion para utilizar un programador de tipo USBASP que utiliza las lineas MOSI,MISO;SCK y RESET del ATMega8


## Programa de prueba
Implementar un programa que barra las 24 columnas con una linea de 5 pixeles que va de izquierda a derecha y luego una linea de 24 pixeles que barra las 5 filas para comprobar el funcionamiento de la matriz.

## Distribucion de los pixeles
En el archivo Matriz_5x24 se muestar como estan conectados los leds en funcion de los drivers de voltaje (C0,C1, hasta C7 ) y los drivers de corriente (F0, F1, hasta F14)

## Hojas de Datos
Las hojas de Datos del driver de corriente (A6282) y el driver de voltaje (FDN302) estan en la carpeta docs
