"Kernel methods for UndefinedObject (nil)."

!UndefinedObject methodsFor: 'testing'!
isNil
	^ true!
notNil
	^ false! !

!UndefinedObject methodsFor: 'printing'!
printString
	^ 'nil'! !

!UndefinedObject methodsFor: 'controlling'!
ifNil: aBlock
	^ aBlock value!
ifNotNil: aBlock
	^ nil!
ifNil: nilBlock ifNotNil: notNilBlock
	^ nilBlock value! !
