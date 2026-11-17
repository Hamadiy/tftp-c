# TFTP client & server en C

![CI](https://github.com/Hamadiy/tftp-c/actions/workflows/ci.yml/badge.svg)
![Langage](https://img.shields.io/badge/langage-C11-blue)
![Plateforme](https://img.shields.io/badge/plateforme-Linux-lightgrey)

Implémentation from scratch du protocole **TFTP (RFC 1350)** : un serveur concurrent et un client en ligne de commande, en C avec les sockets UDP POSIX.

TFTP est utilisé en réseau pour sauvegarder et restaurer les configurations des routeurs et switches Cisco (`copy running-config tftp:`), mettre à jour des firmwares ou faire du boot PXE.

## Fonctionnalités

- **Serveur `tftpd`** : lecture (RRQ) et écriture (WRQ). Chaque client est servi par un processus fils sur un nouveau port UDP (nouveau *Transfer ID*), comme l'exige la RFC. Plusieurs transferts peuvent donc avoir lieu en même temps.
- **Client `tftp`** : commandes `get` et `put`, avec la taille transférée et la durée.
- **Fiabilité sur UDP** : acquittement bloc par bloc (*lock-step*), retransmission après un timeout de 2 s, abandon après 5 essais.
- **Cas limites de la RFC gérés** :
  - un fichier de taille multiple de 512 se termine par un bloc DATA vide ;
  - les ACK dupliqués sont ignorés (correction du bug *Sorcerer's Apprentice*) ;
  - un paquet reçu d'un Transfer ID inconnu reçoit une réponse `ERROR 5` ;
  - le numéro de bloc repasse à 0 après 65535, donc les fichiers de plus de 32 Mo passent.
- **Sécurité du serveur** :
  - pas de sortie du répertoire racine : `..` et `/` sont refusés ;
  - pas d'écrasement d'un fichier existant ;
  - suppression des fichiers partiels si un transfert échoue.

## Format des paquets

```
 RRQ/WRQ  | 01/02 | filename | 0 | mode | 0 |
 DATA     |  03   | block #  |   data (0-512 octets)  |
 ACK      |  04   | block #  |
 ERROR    |  05   | code     | message | 0 |
           2 octets  2 octets
```

## Compilation

```bash
make          # produit bin/tftpd et bin/tftp
make test     # lance les tests d'intégration
```

## Utilisation

```bash
# Serveur (le port 69 demande les droits root, sinon -p 6969)
./bin/tftpd -p 6969 -d /srv/tftp

# Client
./bin/tftp -p 6969 127.0.0.1 put running-config.txt
./bin/tftp -p 6969 127.0.0.1 get running-config.txt backup.txt
```

Exemple de journal côté serveur :

```
tftpd listening on UDP port 6969, serving "/srv/tftp"
[127.0.0.1:51432] WRQ "running-config.txt" (octet)
  -> received "running-config.txt" (2048 bytes)
[127.0.0.1:40211] RRQ "running-config.txt" (octet)
  -> sent "running-config.txt" (2048 bytes)
```

## Tests

`tests/run_tests.sh` démarre le serveur et vérifie avec `cmp` que les fichiers sont identiques après un aller-retour. Il teste plusieurs tailles de fichier (0, 100, 1024 et 300 000 octets) et les cas d'erreur : fichier absent, *path traversal* et écrasement refusé. La CI GitHub Actions compile en `-Werror` et lance ces tests à chaque push.

## Structure

```
src/tftp.h / tftp.c   encodage/décodage des paquets + moteur de transfert
src/tftpd.c           serveur concurrent (fork par requête)
src/tftp_client.c     client en ligne de commande
tests/run_tests.sh    tests d'intégration
```

## Limites

- Le mode `netascii` est accepté, mais les données sont transférées telles quelles, sans conversion des fins de ligne.
- Les extensions de TFTP (RFC 2347/2348 : `blksize`, `tsize`) ne sont pas implémentées.
- IPv4 uniquement.

## Auteur

**Younesse HAMADI** – étudiant en BUT Réseaux & Télécommunications.
