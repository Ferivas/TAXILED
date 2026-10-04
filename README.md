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
Existe un puerto de programacion para utilizar un programador de tipo USBASP que utiliza las lineas MOSI,MISO,SCK y RESET del ATMega8

### Entradas
- PD2=INT0: conmutación de orientación. Un pulsador entre PD2 y GND (con pull-up interno) cambia entre reloj horizontal y vertical en el flanco de bajada (antirrebote de 150ms).
- PD0=RX / PD1=TX: puerto serie 9600 8N1 para sincronizar la hora y ver el registro de actividad.

## Compilación y grabación

### Con PlatformIO
- Compilar: `pio run`
- Grabar (con el USBASP conectado): `pio run -e atmega8 -t upload`

### Con avrdude directamente
Primero compilar con `pio run` para generar `.pio/build/atmega8/firmware.hex` y luego:

```
avrdude -c usbasp -p m8 -B 10 -U flash:w:.pio/build/atmega8/firmware.hex:i
```

Nota: el USBASP (clon) corrompe la escritura a la velocidad de SCK por defecto, por eso siempre se usa `-B 10` (SCK lento). Si el avrdude de PlatformIO falla la verificación, repetir con el avrdude del sistema usando el comando anterior.

Fuse de fábrica correctos para este proyecto: `lfuse=0xC4` (RC interno 8 MHz) y `hfuse=0xDD` (sin bootloader, arranca en 0x0000).

## Operacion
El ATmega8 trabaja con su reloj interno de 8MHz y debe generar interrupciones cada 0,5ms con uno de sus timers (2kHz) para barrer las columnas. En esa interrupcion, primero se apagan todas las columnas y se apagan todas las salidas de driver de corrriente con la señal OE (deshabilitado), para que  luego se envien los datos de las filas serialmente con las señales de SDI y CLK para luego con la señal de LE dejar el dato retenido en el driver con un registro de 16 bits al A6282, luego habilitar el driver serial (OE habilitado) y luego se enciende la columna correspondiente hasta que se produzca otra interrupcion y barrer una nueva columna. repitiendo este proceso hasta completar los 8 drivers de voltaje.
## Programa actual: reloj
El firmware muestra la hora `HH:MM` (formato 24h, arranca en 12:00:50) en la matriz. Los dígitos se dibujan con la fuente 5x4 definida en `docs/Digitos_5x4.ods`. El Timer2 genera el barrido de 2kHz (250 fps) y el Timer1 corre a 200ms (OCR1A=6249, prescaler 256); su interrupción divide entre 5 para avanzar el reloj con una interrupción de 1 segundo y refrescar el framebuffer. Los dos puntos del centro parpadean una vez por segundo. Un LED de señalización en PB2 titila 100ms cada segundo.

### Scroll de los dígitos
Cuando cambia un dígito (siempre en el borde 59→00), un segundo antes del cambio empieza una animación de desplazamiento vertical: en cada intervalo de 200 ms todo el contenido de ese dígito sube una fila (la fila superior sale) y por abajo entra la siguiente fila del dígito nuevo, empezando por su fila 0. Tras los 5 pasos (filas en t-1000, t-800, t-600, t-400 y t-200 ms) han salido todas las filas del dígito antiguo y el dígito nuevo ya está completo, por lo que el refresco del segundo nuevo no produce salto visual. Solo se animan los dígitos cuyo valor cambia (por ejemplo, en 12:59→13:00 se animan los cuatro; en 09:01→09:02 solo las unidades del minuto). El comando `$SETCLK` y el cambio de orientación cancelan la animación en curso.

La orientación del reloj se alterna con un pulso de alto a bajo en PD2 y se guarda en EEPROM, restableciéndose al encender el display:
- **Horizontal**: `HH:MM` en 23 columnas con el formato habitual.
- **Vertical**: dígitos rotados 90° leídos de arriba hacia abajo en el orden `Hh:Mm` (H@cols 19-23, h@13-17, dos puntos@11-12, M@6-10, m@0-4). Los dígitos de los minutos se pintan un píxel más a la derecha (filas 1-4) que los de las horas (filas 0-3).

## Puerto serie
Velocidad **9600 8N1**. Comando para igualar el reloj:

```
$SETCLK,HHMMSS
```

- Debe empezar con `$` y terminar con Enter (CR o LF).
- `HH` horas (00-23), `MM` minutos (00-59), `SS` segundos (00-59). Los segundos no se muestran pero se aplican para que el reloj quede exactamente igualado.

### Registro de actividad (TX)
El firmware envía por PD1 (TX) un registro para depurar:

- Al encender: `TAXILED 9600 READY`
- Por cada línea completa recibida: `RX <línea>` (los caracteres no imprimibles salen como `.`)
- Resultado del procesamiento: `OK` si la hora se aplicó, o uno de estos errores:
  - `ERR LEN`: la línea no tiene exactamente 13 caracteres después del `$`
  - `ERR CMD`: no empieza por `SETCLK,`
  - `ERR DIGIT`: los campos HHMMSS no son todos dígitos
  - `ERR RANGE`: HH fuera de 00-23 o MM/SS fuera de 00-59
  - `ERR LONG`: la línea superó el largo máximo y se descartó

Si no aparece el banner ni ningún `RX`, revisar cableado o baudios; si aparece `RX` con `ERR`, es el formato del comando.

## Distribucion de los pixeles
En el archivo Matriz_5x24 se muestar como estan conectados los leds en funcion de los drivers de voltaje (C0,C1, hasta C7 ) y los drivers de corriente (F0, F1, hasta F14)

## Hojas de Datos
Las hojas de Datos del driver de corriente (A6282) y el driver de voltaje (FDN302) estan en la carpeta docs
