# Manual de Instalación - Driver ms912x (Linux)

Este manual describe cómo instalar y mantener el driver para el adaptador USB a VGA/HDMI de MacroSilicon (`ms912x`) en sistemas Linux, específicamente optimizado para Arch Linux.

## 1. Requisitos Previos

Antes de instalar, asegúrate de tener las herramientas de compilación y los headers de tu kernel actual:

```bash
sudo pacman -S linux-headers dkms base-devel
```

*(Si usas un kernel diferente como `linux-lts` o `linux-zen`, cambia `linux-headers` por `linux-lts-headers` o `linux-zen-headers` respectivamente).*

## 2. Instalación mediante DKMS (Recomendado)

DKMS asegura que el driver se recompile automáticamente cada vez que actualices el kernel.

### Paso A: Limpieza de versiones antiguas (si existen)
Si ya habías intentado instalar una versión anterior que no funciona:
```bash
sudo dkms remove ms912x/0.1 --all
```

### Paso B: Instalación de la versión actual
Desde la carpeta raíz del driver (`/home/mts/dev/ms912x`):
```bash
sudo dkms install .
```

## 3. Carga Automática al Inicio (Persistencia)

Para que el sistema cargue el driver automáticamente al encender el equipo sin tener que ejecutar comandos manuales:

Crea un archivo de configuración en `modules-load.d`:
```bash
echo "ms912x" | sudo tee /etc/modules-load.d/ms912x.conf
```

## 4. Verificación

Para confirmar que el driver se ha cargado correctamente y está funcionando:

1. **Verificar si el módulo está en memoria**:
   ```bash
   lsmod | grep ms912x
   ```

2. **Revisar logs del kernel**:
   ```bash
   dmesg | grep ms912x
   ```

3. **Verificar estado de DKMS**:
   ```bash
   dkms status ms912x
   ```

## 5. Desinstalación

Si necesitas eliminar el driver por completo:
```bash
sudo dkms remove ms912x/0.5.0 --all
sudo rm /etc/modules-load.d/ms912x.conf
```
