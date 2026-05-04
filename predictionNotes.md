on commence non authoritative mais on va se tromper a chaque frame car les moteurs sonts pas deterministes donc correction

si la difference est trop grande on est oblige de tp, le but c'est de jamais tp car c'est degeu.

On a besoin de latence pour que ca serve a quelque chose

normalement en local godot on a un peu de latence
on va pouvoir utiliser clumsy pour ajouter du ping (on peut ajouter 100ms en entree et sortie du serv pour tester)

predire c'est facile, corriger moins, on va faire la version "simple":
moins de 5cm de dif on fait rien
entre 5 cm et 1.5m on corrige
au dessus d'1.5m on snap

le snap c'est facile, on doit juste pas oublier de snap la velocite aussi

frame de prediction (que cote godot), on connais le RTT, la frame serveur, plus un buffer et on peut faire une frame de prediction
quand arrive la frame de pred Fp = Fs + 5 donc 10 => 15
donc a un moment on va recevoir une frame du serveur (frame 10), le serveur joue la frame 10+latence, nous on est en train de predire 15+latence
on recois une frame pos 10;10;10 donc on a genre 7-8 frames d'avance sur le client, donc on va calculer la position du serveur pendant qu4on predit

si on est a 100fps server

on a 80ms de lag entre la frame serveur et la frame pred on peut donc calculer la position 15;15;15 (+velocity*delta)
il faut qu'en un temps relatievment faible on fasse du steering pour converger vers la position serv

donc on applique le ressort: on met un ressort entre nos deux pos (sans calculer la masse, on en a pas besoin)
si on fait que ca, ca va "marcher" mais on va depasser le point et on va avoir du rubber banding
donc exponential backoff
accel=k*dist

avec le backoff:
accel = (K * dist - ((1/e^dist W)(dist/|dist|))*deltaTime

on va rajouter la vitesse et laisser le moteur physique faire la derniere integrale

il faut regler K et W, c'est pas facile, mais il y aun truc:

step 1: w = 0
rubber banding  enorme

on regle k pour plus avoir de rubber banding

ensuite on augmente w, ca va devenir tres lent
donc on va poivoir reaugmenter k

et ainsi de suite jusque ce qu'a ce que ca feel good

deadline jusqu'au 6 mai, il faut juste faire ca

donc avoir une frame de prediction et appliquer une forme de correction

c'est une version tres simple, on a que deux parametres donc ca devrait etre plutot faacile.