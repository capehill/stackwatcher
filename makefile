NAME=stackwatcher

$(NAME): $(NAME).o
	g++ -use-dynld $< -o $(NAME) 

$(NAME).o: $(NAME).cpp
	g++ -O2 -Wall -c $< -o $(NAME).o

strip:
	strip $(NAME)

clean:
	delete $(NAME)
	delete $(NAME).o
